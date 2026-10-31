<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { ResearchService } from '$lib/services/research.service';
	import { errMsg, researchStore as rs, type RunView } from '$lib/stores/research.svelte';
	import type { CounterfactualResult, MetricKey, Observation, RunToken, TokenInfo } from '$lib/types/research';
	import { b64ToHex, buildSeries, fmt, groupColor, isNonUtf8, METRIC_LABELS } from '$lib/utils/research';
	import { SvelteMap } from 'svelte/reactivity';
	import ResearchPiece from './ResearchPiece.svelte';
	import ResearchTrajectoryChart from './ResearchTrajectoryChart.svelte';

	interface Props {
		view: RunView;
	}

	let { view }: Props = $props();

	let metric = $state<MetricKey>('lse');
	let selected = $state<number | null>(null);
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

	let colorByGroup = $derived(new Map(rs.draft.groups.map((g) => [g.id, g.color ?? ''])));
	const colorFor = (id: string, i: number) => colorByGroup.get(id) || groupColor(i);

	let series = $derived(buildSeries(observations, view.groups, metric, colorFor));

	let selToken = $derived(selected === null ? undefined : tokens.find((t) => t.generation_index === selected));
	let selObs = $derived(selected === null ? undefined : obsByIndex.get(selected));
	let priorObs = $derived.by(() => {
		if (selected === null || selObs) return undefined;
		let best: Observation | undefined;
		for (const o of observations) if (o.generation_index < selected && (!best || o.generation_index > best.generation_index)) best = o;
		return best;
	});

	// piece lookup for watched ids (lazy, cached per inspector)
	const pieces = new SvelteMap<number, TokenInfo>();
	$effect(() => {
		const ids = view.watchIds.slice(0, 200).filter((id) => !pieces.has(id));
		if (ids.length === 0) return;
		let cancelled = false;
		void Promise.all(ids.map((id) => ResearchService.token(id).catch(() => null))).then((infos) => {
			if (cancelled) return;
			for (const t of infos) if (t) pieces.set(t.id, t);
		});
		return () => {
			cancelled = true;
		};
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
</script>

<div class="space-y-4">
	<div class="flex flex-wrap items-center gap-2 text-sm">
		<code class="font-medium">{view.id}</code>
		{#if view.manifest}
			<Badge variant="outline">{view.manifest.arm}</Badge>
			<span class="text-xs text-muted-foreground">seed {view.manifest.seed}</span>
		{/if}
		<Badge variant={view.status === 'completed' ? 'default' : view.status === 'running' ? 'secondary' : 'outline'}>{view.status}</Badge>
		{#if view.live}<Badge variant="secondary">live SSE · {view.eventsPerSec} ev/s</Badge>{/if}
		<span class="text-xs">
			captured <b>{view.counters.captured}</b> / skipped <b>{view.counters.skipped}</b> / dropped <b>{view.counters.dropped}</b>
		</span>
		<span class="text-xs text-muted-foreground">tokens {tokens.length}</span>
		<div class="ml-auto flex flex-wrap gap-1">
			{#if view.live}
				<Button size="sm" variant="outline" onclick={togglePause}>{paused ? 'Resume view' : 'Pause view'}</Button>
			{/if}
			{#if view.status === 'running' || view.status === 'queued'}
				<Button size="sm" variant="destructive" onclick={cancel}>Cancel run</Button>
			{/if}
			<Button size="sm" variant="ghost" onclick={() => exportRun('jsonl')}>Export JSONL</Button>
			<Button size="sm" variant="ghost" onclick={() => exportRun('csv')}>Export CSV</Button>
		</div>
	</div>
	{#if view.error || actionError}
		<p class="text-sm text-destructive">{view.error ?? actionError}</p>
	{/if}
	{#if view.manifest}
		<p class="text-xs text-muted-foreground">
			mode {view.manifest.collection?.mode ?? '-'} · every_n {view.manifest.collection?.every_n ?? '-'} · intervention
			{view.manifest.intervention_id ?? 'none (control)'} · model {view.manifest.model_id ?? '-'} · plan {view.manifest.plan_hash ?? '-'}
		</p>
	{/if}

	<div class="grid gap-4 xl:grid-cols-2">
		<section class="space-y-2">
			<h3 class="text-sm font-medium">Generated tokens</h3>
			<p class="text-[10px] text-muted-foreground">Underlined tokens have a captured observation (distribution before that token was sampled). Click to inspect.</p>
			<div class="max-h-80 overflow-y-auto rounded border p-2 font-mono text-sm leading-7">
				{#each tokens as t (t.generation_index)}
					<button
						class={[
							'rounded px-0.5 whitespace-pre hover:bg-muted',
							obsByIndex.has(t.generation_index) && 'underline decoration-primary decoration-2 underline-offset-4',
							selected === t.generation_index && 'bg-accent'
						]}
						title="#{t.generation_index} id {t.token_id}"
						onclick={() => (selected = t.generation_index)}>{isNonUtf8(t.piece_b64) ? `<${b64ToHex(t.piece_b64)}>` : t.piece}</button
					>
				{:else}
					<span class="text-xs text-muted-foreground">no tokens yet</span>
				{/each}
			</div>
		</section>

		<section class="space-y-2">
			<div class="flex flex-wrap items-center gap-1">
				<h3 class="mr-2 text-sm font-medium">Group trajectories</h3>
				{#each Object.keys(METRIC_LABELS) as m (m)}
					<Button size="sm" variant={metric === m ? 'secondary' : 'ghost'} onclick={() => (metric = m as MetricKey)}>{METRIC_LABELS[m as MetricKey]}</Button>
				{/each}
			</div>
			<ResearchTrajectoryChart {series} selectedX={selected} onSelect={(x) => (selected = x)} />
		</section>
	</div>

	<section class="space-y-2 rounded-lg border p-3 text-sm">
		<h3 class="font-medium">Token inspector</h3>
		{#if selected === null}
			<p class="text-xs text-muted-foreground">Click a token or a chart position.</p>
		{:else}
			<div class="flex flex-wrap items-center gap-3">
				<span>generation_index <b>{selected}</b></span>
				{#if selToken}
					<span>token id <b>{selToken.token_id}</b></span>
					<span>piece <ResearchPiece piece={selToken.piece} b64={selToken.piece_b64} /></span>
					{#if selToken.piece_b64}<span class="text-xs text-muted-foreground">bytes {b64ToHex(selToken.piece_b64)} · b64 {selToken.piece_b64}</span>{/if}
					{#if selToken.context_position !== undefined}<span class="text-xs text-muted-foreground">ctx pos {selToken.context_position}</span>{/if}
				{:else}
					<span class="text-xs text-muted-foreground">no accepted token at this index</span>
				{/if}
			</div>

			{#if !selObs}
				<p class="rounded bg-muted p-2 text-sm font-medium">No data at this position (not sampled).</p>
				{#if priorObs}
					<p class="text-xs text-muted-foreground">
						Nearest prior captured sample is at generation_index {priorObs.generation_index} (a different context, not this position).
						<Button size="sm" variant="link" onclick={() => (selected = priorObs!.generation_index)}>Go to it</Button>
					</p>
				{/if}
			{:else}
				<p class="text-xs text-muted-foreground">
					Distribution before token {selected} was sampled. raw = model logits; post_intervention = raw + effective bias
					(computed; later samplers not reflected). capture_state {selObs.capture_state ?? '-'}
					{#if selObs.raw_log_z !== null}· raw_log_z {fmt(selObs.raw_log_z)} · adjusted_log_z {fmt(selObs.adjusted_log_z)}{/if}
				</p>
				<div class="grid gap-4 lg:grid-cols-2">
					<table class="w-full text-xs">
						<thead class="text-left text-muted-foreground">
							<tr><th>group</th><th>raw lse</th><th>post lse</th><th>raw rel</th><th>post rel</th><th>raw abs</th><th>post abs</th></tr>
						</thead>
						<tbody>
							{#each selObs.groups as g, gi (g.id)}
								<tr class="border-t font-mono">
									<td style:color={colorFor(g.id, gi)}>{view.groups.find((x) => x.id === g.id)?.name ?? g.id}</td>
									<td>{fmt(g.raw_lse)}</td><td>{fmt(g.adj_lse)}</td>
									<td>{fmt(g.raw_rel)}</td><td>{fmt(g.adj_rel)}</td>
									<td>{fmt(g.raw_abs)}</td><td>{fmt(g.adj_abs)}</td>
								</tr>
							{/each}
						</tbody>
					</table>
					<div class="max-h-64 overflow-y-auto">
						<table class="w-full text-xs">
							<thead class="text-left text-muted-foreground">
								<tr><th>watched id</th><th>piece</th><th>raw logit</th><th>post_intervention</th></tr>
							</thead>
							<tbody>
								{#each view.watchIds as id, i (id)}
									<tr class={['border-t font-mono', selObs.selected_token_id === id && 'bg-accent']}>
										<td>{id}</td>
										<td>{#if pieces.get(id)}<ResearchPiece piece={pieces.get(id)!.piece} b64={pieces.get(id)!.piece_b64} />{/if}</td>
										<td>{fmt(selObs.raw[i])}</td>
										<td>{fmt(selObs.adjusted[i])}</td>
									</tr>
								{/each}
							</tbody>
						</table>
					</div>
				</div>
				{#if selObs.top_k?.length}
					<div class="text-xs">
						<span class="text-muted-foreground">top-k discovery (raw):</span>
						{#each selObs.top_k as t (t.id)}<span class="ml-2 font-mono">{t.id}:{fmt(t.logit, 2)}</span>{/each}
					</div>
				{/if}

				<div class="space-y-2 border-t pt-2">
					<div class="flex flex-wrap items-center gap-2">
						<span class="font-medium">Counterfactual (same prefix)</span>
						<select bind:value={cfIntervention} class="h-8 rounded-md border border-input bg-transparent px-2 text-sm dark:bg-input/30">
							<option value="" disabled>intervention</option>
							{#each rs.interventions as iv (iv.id)}<option value={iv.id}>{iv.id}</option>{/each}
						</select>
						<Button size="sm" variant="outline" onclick={runCounterfactual} disabled={!cfIntervention}>Compute</Button>
					</div>
					<p class="text-[10px] text-muted-foreground">
						Recomputed server side from the stored raw logits at this exact position: the mechanical effect of the
						intervention on the same context, not a new rollout.
					</p>
					{#if cfError}<p class="text-xs text-destructive">{cfError}</p>{/if}
					{#if cf}
						<div class="grid gap-3 md:grid-cols-2">
							{#each [['raw', cf.raw?.groups], ['post_intervention', cf.adjusted?.groups]] as [label, groups] (label)}
								<div>
									<h4 class="text-xs font-medium">{label}</h4>
									{#each cfRows(groups) as row (row.id)}
										<div class="font-mono text-xs">
											{row.id}: {#each row.vals as [k, v] (k)}<span class="ml-2">{k}={num(v)}</span>{/each}
										</div>
									{/each}
								</div>
							{/each}
						</div>
					{/if}
				</div>
			{/if}
		{/if}
	</section>
</div>
