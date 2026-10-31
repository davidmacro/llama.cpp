<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { ResearchService } from '$lib/services/research.service';
	import { errMsg, researchStore as rs, type RunView } from '$lib/stores/research.svelte';
	import type { CounterfactualResult, MetricKey, Observation, RunToken } from '$lib/types/research';
	import { b64ToHex, buildSeries, fmt, groupColor, isNonUtf8, METRIC_LABELS, ruleSummary, SELECT_CLASS } from '$lib/utils/research';
	import ResearchDistributionCard from './ResearchDistributionCard.svelte';
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
	let selObs = $derived(selected === null ? undefined : obsByIndex.get(selected));

	let ivLabel = $derived.by(() => {
		const id = view.manifest?.intervention_id;
		if (!id) return 'control';
		const iv = rs.interventions.find((i) => i.id === id);
		return iv ? ruleSummary(iv.rules, (g) => rs.groupName(g)) : id;
	});

	// piece lookup for watched and top-k ids (shared cache, fetched once)
	$effect(() => {
		rs.ensureTokens(view.watchIds.slice(0, 200));
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

	<div class="grid items-start gap-3 lg:grid-cols-2">
		<div class="min-w-0 space-y-3">
			<section class="space-y-1">
				<h3 class="text-sm font-medium">
					Generated answer <span class="text-[10px] font-normal text-muted-foreground">underlined = captured; click to inspect</span>
				</h3>
				<div class="max-h-40 overflow-y-auto rounded border p-2 font-mono text-sm leading-7">
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

			{#if selObs}
				<section class="max-h-72 overflow-y-auto rounded-lg border p-2">
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
			{/if}
		</div>

		<div class="min-w-0 space-y-3">
			<ResearchDistributionCard {view} {tokens} {observations} {selected} onPick={(x) => (picked = x)} />

			{#if selObs}
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
			{/if}
		</div>
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
</div>
