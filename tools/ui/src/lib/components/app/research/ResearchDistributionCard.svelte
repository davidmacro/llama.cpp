<script lang="ts">
	import { Button } from '$lib/components/ui/button';
	import { researchStore as rs, type RunView } from '$lib/stores/research.svelte';
	import type { GroupMetric, Observation, RunToken } from '$lib/types/research';
	import { b64ToHex, fmt, fmtMass, groupColor, logOdds, unwatchedMass } from '$lib/utils/research';
	import type { Snippet } from 'svelte';
	import ResearchAddToGroup from './ResearchAddToGroup.svelte';
	import ResearchPiece from './ResearchPiece.svelte';

	interface Props {
		/** Extra header content (e.g. a run label in compare). */
		header?: Snippet;
		observations: Observation[];
		onPick?: (generationIndex: number) => void;
		selected: number | null;
		tokens: RunToken[];
		view: RunView;
	}

	let { header, observations, onPick, selected, tokens, view }: Props = $props();

	let colorByGroup = $derived(new Map(rs.draft.groups.map((g) => [g.id, g.color ?? ''])));
	const colorFor = (id: string, i: number) => colorByGroup.get(id) || groupColor(i);
	const nameOf = (id: string) => view.groups.find((g) => g.id === id)?.name ?? rs.groupName(id);

	let selToken = $derived(selected === null ? undefined : tokens.find((t) => t.generation_index === selected));
	let selObs = $derived(selected === null ? undefined : observations.find((o) => o.generation_index === selected));
	let priorObs = $derived.by(() => {
		if (selected === null || selObs) return undefined;
		let best: Observation | undefined;
		for (const o of observations) if (o.generation_index < selected && (!best || o.generation_index > best.generation_index)) best = o;
		return best;
	});

	// absolute mass when the run captured log Z, else watch-relative
	let massKind = $derived<'abs' | 'rel'>(selObs?.groups.some((g) => g.raw_abs !== null) ? 'abs' : 'rel');

	interface StageStats {
		winner: string | null;
		margin: number | null;
		logOdds: number | null;
	}

	function stageStats(groups: GroupMetric[], stage: 'raw' | 'adj'): StageStats {
		const rows = groups
			.map((g) => ({ id: g.id, lse: g[`${stage}_lse`], mass: g[`${stage}_${massKind}`] }))
			.filter((r): r is { id: string; lse: number; mass: number | null } => typeof r.lse === 'number' && Number.isFinite(r.lse))
			.sort((a, b) => b.lse - a.lse);
		if (rows.length === 0) return { winner: null, margin: null, logOdds: null };
		return {
			winner: rows[0].id,
			margin: rows.length > 1 ? rows[0].lse - rows[1].lse : null,
			logOdds: logOdds(rows[0].mass)
		};
	}

	let stats = $derived(selObs ? { raw: stageStats(selObs.groups, 'raw'), adj: stageStats(selObs.groups, 'adj') } : null);

	// mass outside every watched group (only with full log Z)
	let unwatched = $derived.by(() => {
		const raw = unwatchedMass(selObs, 'raw');
		const adj = unwatchedMass(selObs, 'adj');
		return raw === null || adj === null ? null : { raw, adj };
	});

	// compiled membership: token id -> group ids (from the run's manifest)
	let memberOf = $derived.by(() => {
		const m = new Map<number, string[]>();
		for (const g of view.manifest?.compiled?.groups ?? []) for (const id of g.members) m.set(id, [...(m.get(id) ?? []), g.id]);
		return m;
	});
	let selOutside = $derived(
		!!selObs && selObs.selected_token_id !== undefined && memberOf.size > 0 && !memberOf.has(selObs.selected_token_id)
	);

	// pieces for the top-k ids (shared cache, fetched once)
	$effect(() => {
		if (selObs?.top_k) rs.ensureTokens(selObs.top_k.map((t) => t.id));
	});

	const pct = (m: number | null) => (m === null || !Number.isFinite(m) ? 0 : Math.max(0, Math.min(1, m)) * 100);
</script>

<section class="min-w-0 space-y-2 rounded-lg border p-3 text-sm">
	<div class="flex flex-wrap items-center gap-2">
		{@render header?.()}
		{#if selected === null}
			<h3 class="font-medium">No captured position yet</h3>
		{:else}
			<h3 class="font-medium">Position {selected}</h3>
			{#if selToken}
				<span class="inline-flex items-center gap-0.5">
					<ResearchPiece piece={selToken.piece} b64={selToken.piece_b64} />
					<ResearchAddToGroup tokenId={selToken.token_id} />
				</span>
				<span class="text-xs text-muted-foreground" title={selToken.piece_b64 ? `bytes ${b64ToHex(selToken.piece_b64)}` : ''}
					>id {selToken.token_id}{selToken.context_position !== undefined ? ` \u00b7 ctx ${selToken.context_position}` : ''}</span
				>
			{/if}
			{#if selOutside}
				<span class="rounded bg-amber-500/15 px-1 text-[10px] text-amber-600" title="Selected token id {selObs?.selected_token_id} is in no group"
					>in no group</span
				>
			{/if}
		{/if}
	</div>

	{#if selected !== null && !selObs}
		<p class="rounded bg-muted p-2 text-sm font-medium">No data at this position (not sampled).</p>
		{#if priorObs && onPick}
			<Button size="sm" variant="link" class="h-auto p-0 text-xs" onclick={() => onPick(priorObs!.generation_index)}
				>Go to nearest prior sample ({priorObs.generation_index}, a different context)</Button
			>
		{/if}
	{:else if selObs && stats}
		<table class="w-full text-xs">
			<thead class="text-left text-muted-foreground">
				<tr><th></th><th>raw</th><th>post_intervention</th></tr>
			</thead>
			<tbody class="font-mono">
				<tr class="border-t">
					<td class="py-0.5 font-sans text-muted-foreground" title="watched group with the highest logsumexp">top watched group</td>
					{#each [stats.raw, stats.adj] as s, i (i)}
						<td class="font-sans font-medium" style:color={s.winner ? colorFor(s.winner, 0) : undefined}>{s.winner ? nameOf(s.winner) : '-'}</td>
					{/each}
				</tr>
				<tr class="border-t">
					<td class="py-0.5 font-sans text-muted-foreground" title="top group lse minus runner-up lse">logit margin</td>
					<td>{fmt(stats.raw.margin, 2)}</td><td>{fmt(stats.adj.margin, 2)}</td>
				</tr>
				<tr class="border-t">
					<td
						class="py-0.5 font-sans text-muted-foreground"
						title="ln(p / (1 - p)) of the top watched group ({massKind === 'abs' ? 'absolute' : 'watch-relative'} mass)">log-odds</td
					>
					<td>{fmt(stats.raw.logOdds, 2)}</td><td>{fmt(stats.adj.logOdds, 2)}</td>
				</tr>
			</tbody>
		</table>

		<div class="space-y-1.5">
			<div class="text-[10px] text-muted-foreground">
				{massKind === 'abs' ? 'Absolute mass (full softmax)' : 'Watch-relative mass (share of watched tokens)'}: solid = raw, light =
				post_intervention
			</div>
			{#each selObs.groups as g, gi (g.id)}
				{@const raw = g[`raw_${massKind}`]}
				{@const adj = g[`adj_${massKind}`]}
				<div class="grid grid-cols-[6rem_1fr_4.5rem] items-center gap-x-2 text-xs">
					<span class="row-span-2 truncate font-medium" style:color={colorFor(g.id, gi)} title={g.id}>{nameOf(g.id)}</span>
					<div class="h-2 rounded bg-muted"><div class="h-2 rounded" style:width="{pct(raw)}%" style:background={colorFor(g.id, gi)}></div></div>
					<span class="text-right font-mono">{fmtMass(raw)}</span>
					<div class="h-2 rounded bg-muted">
						<div class="h-2 rounded opacity-40" style:width="{pct(adj)}%" style:background={colorFor(g.id, gi)}></div>
					</div>
					<span class="text-right font-mono text-muted-foreground">{fmtMass(adj)}</span>
				</div>
			{/each}
			{#if unwatched}
				<div class="grid grid-cols-[6rem_1fr_4.5rem] items-center gap-x-2 text-xs" title="1 - sum of absolute group masses (tokens in no group)">
					<span class={['row-span-2 truncate italic', Math.max(unwatched.raw, unwatched.adj) > 0.5 ? 'text-amber-600' : 'text-muted-foreground']}
						>other (unwatched)</span
					>
					<div class="h-2 rounded bg-muted"><div class="h-2 rounded bg-muted-foreground/60" style:width="{pct(unwatched.raw)}%"></div></div>
					<span class="text-right font-mono">{fmtMass(unwatched.raw)}</span>
					<div class="h-2 rounded bg-muted"><div class="h-2 rounded bg-muted-foreground/30" style:width="{pct(unwatched.adj)}%"></div></div>
					<span class="text-right font-mono text-muted-foreground">{fmtMass(unwatched.adj)}</span>
				</div>
			{/if}
		</div>

		{#if selObs.top_k?.length}
			<div class="flex flex-wrap items-center gap-1 text-xs">
				<span class="text-muted-foreground">top-k (raw):</span>
				{#each selObs.top_k as t (t.id)}
					{@const info = rs.tokenCache.get(t.id)}
					<span
						class={['inline-flex items-center gap-0.5 rounded border pl-1', !memberOf.has(t.id) && memberOf.size > 0 && 'border-dashed']}
						title="id {t.id}{memberOf.has(t.id) ? '' : ' (in no group)'}"
					>
						{#if info}<ResearchPiece piece={info.piece} b64={info.piece_b64} />{:else}#{t.id}{/if}
						<span class="font-mono text-muted-foreground">{fmt(t.logit, 1)}</span>
						<ResearchAddToGroup tokenId={t.id} />
					</span>
				{/each}
			</div>
		{/if}
	{/if}
</section>
