<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { ResearchService } from '$lib/services/research.service';
	import { errMsg, researchStore as rs, type RunView } from '$lib/stores/research.svelte';
	import type { CounterfactualResult, GroupMetric, MetricKey, Observation, RunToken } from '$lib/types/research';
	import {
		b64ToHex,
		buildSeries,
		fmt,
		fmtMass,
		groupColor,
		isNonUtf8,
		logOdds,
		METRIC_LABELS,
		ruleSummary,
		SELECT_CLASS
	} from '$lib/utils/research';
	import ResearchAddToGroup from './ResearchAddToGroup.svelte';
	import ResearchPiece from './ResearchPiece.svelte';
	import ResearchTrajectoryChart from './ResearchTrajectoryChart.svelte';

	interface Props {
		onReload?: () => void;
		view: RunView;
	}

	let { onReload, view }: Props = $props();

	let metric = $state<MetricKey>('lse');
	let picked = $state<number | null>(null);
	let paused = $state(false);
	let frozen = $state.raw<{ tokens: RunToken[]; observations: Observation[] } | null>(null);
	let cfIntervention = $state('');
	let cf = $state.raw<CounterfactualResult | null>(null);
	let cfError = $state<string | null>(null);
	let actionError = $state<string | null>(null);

	// pausing freezes the rendered snapshot; the run keeps generating server side
	let tokens = $derived(paused && frozen ? frozen.tokens : view.tokens);
	let observations = $derived(paused && frozen ? frozen.observations : view.observations);
	let obsByIndex = $derived(new Map(observations.map((o) => [o.generation_index, o])));

	// default to the first captured position until the user picks one
	let selected = $derived(picked ?? observations[0]?.generation_index ?? null);

	let colorByGroup = $derived(new Map(rs.draft.groups.map((g) => [g.id, g.color ?? ''])));
	const colorFor = (id: string, i: number) => colorByGroup.get(id) || groupColor(i);
	const nameOf = (id: string) => view.groups.find((g) => g.id === id)?.name ?? rs.groupName(id);

	let series = $derived(buildSeries(observations, view.groups, metric, colorFor));

	let selToken = $derived(selected === null ? undefined : tokens.find((t) => t.generation_index === selected));
	let selObs = $derived(selected === null ? undefined : obsByIndex.get(selected));
	let priorObs = $derived.by(() => {
		if (selected === null || selObs) return undefined;
		let best: Observation | undefined;
		for (const o of observations) if (o.generation_index < selected && (!best || o.generation_index > best.generation_index)) best = o;
		return best;
	});

	let ivLabel = $derived.by(() => {
		const id = view.manifest?.intervention_id;
		if (!id) return 'control';
		const iv = rs.interventions.find((i) => i.id === id);
		return iv ? ruleSummary(iv.rules, (g) => rs.groupName(g)) : id;
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

	// mass outside every watched group (only meaningful with full log Z)
	let unwatched = $derived.by(() => {
		if (!selObs || massKind !== 'abs') return null;
		const rest = (stage: 'raw' | 'adj') => {
			let sum = 0;
			for (const g of selObs.groups) {
				const v = g[`${stage}_abs`];
				if (typeof v === 'number' && Number.isFinite(v)) sum += v;
			}
			return Math.max(0, 1 - sum);
		};
		return { raw: rest('raw'), adj: rest('adj') };
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

	let stats = $derived(selObs ? { raw: stageStats(selObs.groups, 'raw'), adj: stageStats(selObs.groups, 'adj') } : null);

	// piece lookup for watched and top-k ids (shared cache, fetched once)
	$effect(() => {
		const ids = view.watchIds.slice(0, 200);
		if (selObs?.top_k) ids.push(...selObs.top_k.map((t) => t.id));
		rs.ensureTokens(ids);
	});

	function togglePause() {
		if (!paused) frozen = { tokens: view.tokens, observations: view.observations };
		paused = !paused;
	}

	async function exportRun(format: 'jsonl' | 'csv') {
		actionError = null;
		try {
			await ResearchService.exportRun(view.id, format);
		} catch (e) {
			actionError = errMsg(e);
		}
	}

	async function cancel() {
		actionError = null;
		try {
			await ResearchService.cancelRun(view.id);
		} catch (e) {
			actionError = errMsg(e);
		}
	}

	async function runCounterfactual() {
		if (selected === null || !cfIntervention) return;
		cfError = null;
		cf = null;
		try {
			cf = await ResearchService.counterfactual(view.id, selected, cfIntervention);
		} catch (e) {
			cfError = errMsg(e);
		}
	}

	function cfRows(groups: unknown): { id: string; vals: [string, unknown][] }[] {
		if (Array.isArray(groups))
			return groups.map((g, i) => {
				const { id, ...rest } = g as Record<string, unknown>;
				return { id: String(id ?? i), vals: Object.entries(rest) };
			});
		if (groups && typeof groups === 'object')
			return Object.entries(groups as Record<string, unknown>).map(([id, v]) => ({
				id,
				vals: v && typeof v === 'object' ? Object.entries(v as Record<string, unknown>) : [['value', v]]
			}));
		return [];
	}

	function num(v: unknown): string {
		return typeof v === 'number' ? fmt(v, 4) : v === null ? '-' : String(v);
	}

	const pct = (m: number | null) => (m === null || !Number.isFinite(m) ? 0 : Math.max(0, Math.min(1, m)) * 100);
</script>

<div class="space-y-3">
	<div class="flex flex-wrap items-center gap-2 text-sm">
		<code class="font-medium" title={view.id}>{view.id.slice(-8)}</code>
		{#if view.manifest}
			<Badge variant="outline">{view.manifest.arm}</Badge>
			<span class="text-xs text-muted-foreground">seed {view.manifest.seed}</span>
		{/if}
		<Badge variant={view.status === 'completed' ? 'default' : view.status === 'running' ? 'secondary' : 'outline'}>{view.status}</Badge>
		{#if view.live}<Badge variant="secondary">live &middot; {view.eventsPerSec} ev/s</Badge>{/if}
		<span class="text-xs text-muted-foreground">
			{tokens.length} tokens &middot; captured {view.counters.captured} / skipped {view.counters.skipped} / dropped {view.counters.dropped}
			{#if view.manifest}
				&middot; {view.manifest.collection?.mode ?? '-'} every {view.manifest.collection?.every_n ?? '-'} &middot; {ivLabel}
			{/if}
		</span>
		<div class="ml-auto flex flex-wrap gap-1">
			{#if view.live}
				<Button size="sm" variant="outline" onclick={togglePause}>{paused ? 'Resume view' : 'Pause view'}</Button>
			{/if}
			{#if view.status === 'running' || view.status === 'queued'}
				<Button size="sm" variant="destructive" onclick={cancel}>Cancel run</Button>
			{/if}
			{#if onReload}<Button size="sm" variant="ghost" onclick={onReload} title="Reload manifest, tokens and observations">Reload</Button>{/if}
			<Button size="sm" variant="ghost" onclick={() => exportRun('jsonl')}>JSONL</Button>
			<Button size="sm" variant="ghost" onclick={() => exportRun('csv')}>CSV</Button>
		</div>
	</div>
	{#if view.error || actionError}
		<p class="text-sm text-destructive">{view.error ?? actionError}</p>
	{/if}

	<div class="grid gap-3 lg:grid-cols-2">
		<section class="min-w-0 space-y-1">
			<h3 class="text-sm font-medium">
				Generated answer <span class="text-[10px] font-normal text-muted-foreground">underlined = captured; click to inspect</span>
			</h3>
			<div class="max-h-56 min-h-[5.75rem] overflow-y-auto rounded border p-2 font-mono text-sm leading-7">
				{#each tokens as t (t.generation_index)}
					<button
						class={[
							'rounded px-0.5 whitespace-pre hover:bg-muted',
							obsByIndex.has(t.generation_index) && 'underline decoration-primary decoration-2 underline-offset-4',
							selected === t.generation_index && 'bg-accent ring-1 ring-primary/50'
						]}
						title="#{t.generation_index} id {t.token_id}"
						onclick={() => (picked = t.generation_index)}>{isNonUtf8(t.piece_b64) ? `<${b64ToHex(t.piece_b64)}>` : t.piece}</button
					>
				{:else}
					<span class="text-xs text-muted-foreground">no tokens yet</span>
				{/each}
			</div>
		</section>

		<section class="min-w-0 space-y-2 rounded-lg border p-3 text-sm">
			{#if selected === null}
				<p class="text-xs text-muted-foreground">No captured position yet.</p>
			{:else}
				<div class="flex flex-wrap items-center gap-2">
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
				</div>

				{#if !selObs}
					<p class="rounded bg-muted p-2 text-sm font-medium">No data at this position (not sampled).</p>
					{#if priorObs}
						<Button size="sm" variant="link" class="h-auto p-0 text-xs" onclick={() => (picked = priorObs!.generation_index)}
							>Go to nearest prior sample ({priorObs.generation_index}, a different context)</Button
						>
					{/if}
				{:else if stats}
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
								<td class="py-0.5 font-sans text-muted-foreground" title="ln(p / (1 - p)) of the top watched group ({massKind === 'abs' ? 'absolute' : 'watch-relative'} mass)"
									>log-odds</td
								>
								<td>{fmt(stats.raw.logOdds, 2)}</td><td>{fmt(stats.adj.logOdds, 2)}</td>
							</tr>
						</tbody>
					</table>

					<div class="space-y-1.5">
						<div class="text-[10px] text-muted-foreground">
							{massKind === 'abs' ? 'Absolute mass (full softmax)' : 'Watch-relative mass (share of watched tokens)'}: solid = raw, light = post_intervention
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
								<span class="row-span-2 truncate text-muted-foreground italic">other (unwatched)</span>
								<div class="h-2 rounded bg-muted"><div class="h-2 rounded bg-muted-foreground/60" style:width="{pct(unwatched.raw)}%"></div></div>
								<span class="text-right font-mono">{fmtMass(unwatched.raw)}</span>
								<div class="h-2 rounded bg-muted"><div class="h-2 rounded bg-muted-foreground/30" style:width="{pct(unwatched.adj)}%"></div></div>
								<span class="text-right font-mono text-muted-foreground">{fmtMass(unwatched.adj)}</span>
							</div>
						{/if}
					</div>
					{#if unwatched && Math.max(unwatched.raw, unwatched.adj) > 0.5}
						<p class="text-xs text-amber-600">
							Over half the mass is unwatched (raw {fmtMass(unwatched.raw)}, post {fmtMass(unwatched.adj)}): group shares cover a minority.
						</p>
					{/if}
					{#if selOutside}
						<p class="text-xs text-amber-600">Selected token outside all groups (id {selObs.selected_token_id}).</p>
					{/if}

					{#if selObs.top_k?.length}
						<div class="flex flex-wrap items-center gap-1 text-xs">
							<span class="text-muted-foreground">top-k (raw):</span>
							{#each selObs.top_k as t (t.id)}
								{@const info = rs.tokenCache.get(t.id)}
								<span class={['inline-flex items-center gap-0.5 rounded border pl-1', !memberOf.has(t.id) && memberOf.size > 0 && 'border-dashed']} title="id {t.id}{memberOf.has(t.id) ? '' : ' (in no group)'}">
									{#if info}<ResearchPiece piece={info.piece} b64={info.piece_b64} />{:else}#{t.id}{/if}
									<span class="font-mono text-muted-foreground">{fmt(t.logit, 1)}</span>
									<ResearchAddToGroup tokenId={t.id} />
								</span>
							{/each}
						</div>
					{/if}
				{/if}
			{/if}
		</section>
	</div>

	{#if observations.length < 3}
		<p class="text-xs text-muted-foreground">
			Group trajectories need at least 3 captured positions (this run has {observations.length}).
		</p>
	{:else}
	<section class="space-y-1">
		<div class="flex flex-wrap items-center gap-1">
			<h3 class="mr-2 text-sm font-medium">Group trajectories</h3>
			{#each Object.keys(METRIC_LABELS) as m (m)}
				<Button size="sm" variant={metric === m ? 'secondary' : 'ghost'} onclick={() => (metric = m as MetricKey)}>{METRIC_LABELS[m as MetricKey]}</Button>
			{/each}
		</div>
		<ResearchTrajectoryChart {series} selectedX={selected} onSelect={(x) => (picked = x)} />
	</section>
	{/if}

	{#if selObs}
		<div class="grid gap-3 lg:grid-cols-2">
			<section class="max-h-64 overflow-y-auto rounded-lg border p-2">
				<h3 class="mb-1 text-sm font-medium">Watched logits at position {selected}</h3>
				<table class="w-full text-xs">
					<thead class="text-left text-muted-foreground">
						<tr><th>id</th><th>piece</th><th>raw logit</th><th>post_intervention</th></tr>
					</thead>
					<tbody>
						{#each view.watchIds as id, i (id)}
							{@const info = rs.tokenCache.get(id)}
							<tr class={['border-t font-mono', selObs.selected_token_id === id && 'bg-accent']}>
								<td>{id}</td>
								<td>{#if info}<ResearchPiece piece={info.piece} b64={info.piece_b64} />{/if}</td>
								<td>{fmt(selObs.raw[i])}</td>
								<td>{fmt(selObs.adjusted[i])}</td>
							</tr>
						{/each}
					</tbody>
				</table>
			</section>

			<section class="space-y-2 rounded-lg border p-2 text-sm">
				<div class="flex flex-wrap items-center gap-2">
					<h3 class="font-medium" title="Recomputed from stored raw logits at this exact position: same context, not a new rollout">
						Counterfactual (same prefix)
					</h3>
					<select bind:value={cfIntervention} class={SELECT_CLASS}>
						<option value="" disabled>intervention</option>
						{#each rs.interventions as iv (iv.id)}<option value={iv.id}>{ruleSummary(iv.rules, (g) => rs.groupName(g))}</option>{/each}
					</select>
					<Button size="sm" variant="outline" onclick={runCounterfactual} disabled={!cfIntervention}>Compute</Button>
				</div>
				{#if cfError}<p class="text-xs text-destructive">{cfError}</p>{/if}
				{#if cf}
					<div class="grid gap-3 md:grid-cols-2">
						{#each [['raw', cf.raw?.groups], ['post_intervention', cf.adjusted?.groups]] as [label, groups] (label)}
							<div>
								<h4 class="text-xs font-medium">{label}</h4>
								{#each cfRows(groups) as row (row.id)}
									<div class="font-mono text-xs">
										{nameOf(row.id)}: {#each row.vals as [k, v] (k)}<span class="ml-2">{k}={num(v)}</span>{/each}
									</div>
								{/each}
							</div>
						{/each}
					</div>
				{/if}
			</section>
		</div>
	{/if}
</div>
