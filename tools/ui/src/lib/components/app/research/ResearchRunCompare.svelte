<script lang="ts">
	import { Button } from '$lib/components/ui/button';
	import { researchStore as rs, type RunView } from '$lib/stores/research.svelte';
	import type { MetricKey, RunToken } from '$lib/types/research';
	import { b64ToHex, buildSeries, groupColor, isNonUtf8, METRIC_LABELS, SELECT_CLASS } from '$lib/utils/research';
	import ResearchDistributionCard from './ResearchDistributionCard.svelte';
	import ResearchTrajectoryChart from './ResearchTrajectoryChart.svelte';

	let metric = $state<MetricKey>('lse');

	let viewA = $derived(rs.compareIds[0] ? rs.views.get(rs.compareIds[0]) : undefined);
	let viewB = $derived(rs.compareIds[1] ? rs.views.get(rs.compareIds[1]) : undefined);

	function pick(slot: 0 | 1, id: string) {
		if (id) rs.openRun(id);
		const next: [string | null, string | null] = [...rs.compareIds];
		next[slot] = id || null;
		rs.compareIds = next;
	}

	// first generation_index where the sampled token ids differ
	let divergeAt = $derived.by(() => {
		if (!viewA || !viewB) return null;
		const a = viewA.tokens;
		const b = viewB.tokens;
		const n = Math.min(a.length, b.length);
		for (let i = 0; i < n; i++) if (a[i].token_id !== b[i].token_id) return a[i].generation_index;
		return null;
	});

	let colorByGroup = $derived(new Map(rs.draft.groups.map((g) => [g.id, g.color ?? ''])));
	const colorFor = (id: string, i: number) => colorByGroup.get(id) || groupColor(i);

	let seriesA = $derived(viewA ? buildSeries(viewA.observations, viewA.groups, metric, colorFor) : []);
	let seriesB = $derived(viewB ? buildSeries(viewB.observations, viewB.groups, metric, colorFor) : []);

	// one shared x and y domain for both charts
	let range = $derived.by(() => {
		let xMax = 1;
		let yMin = Infinity;
		let yMax = -Infinity;
		for (const s of [...seriesA, ...seriesB])
			for (const p of s.points) {
				xMax = Math.max(xMax, p.x);
				yMin = Math.min(yMin, p.y);
				yMax = Math.max(yMax, p.y);
			}
		if (!Number.isFinite(yMin)) return { xMax, yMin: 0, yMax: 1 };
		if (yMax - yMin < 1e-9) return { xMax, yMin: yMin - 0.5, yMax: yMax + 0.5 };
		return { xMax, yMin, yMax };
	});

	const hasPos0 = (v: RunView | undefined) => !!v?.observations.some((o) => o.generation_index === 0);

	// same prefix = identical prompt + rendered prompt and position 0 captured in both runs
	let samePrefix = $derived.by(() => {
		const a = viewA?.manifest;
		const b = viewB?.manifest;
		if (!a || !b) return null;
		return (a.prompt ?? '') === (b.prompt ?? '') && (a.rendered_prompt ?? '') === (b.rendered_prompt ?? '') && hasPos0(viewA) && hasPos0(viewB);
	});

	let showCharts = $derived(!!viewA && !!viewB && viewA.observations.length >= 3 && viewB.observations.length >= 3);

	function label(t: RunToken): string {
		return isNonUtf8(t.piece_b64) ? `<${b64ToHex(t.piece_b64)}>` : t.piece;
	}
</script>

{#snippet picker(slot: 0 | 1)}
	<div class="flex min-w-0 items-center gap-2">
		<span class="w-4 text-sm font-semibold">{slot === 0 ? 'A' : 'B'}</span>
		<select class={[SELECT_CLASS, 'min-w-0 flex-1']} value={rs.compareIds[slot] ?? ''} onchange={(e) => pick(slot, e.currentTarget.value)}>
			<option value="">select run {slot === 0 ? 'A' : 'B'}</option>
			{#each rs.runs as r (r.id)}
				<option value={r.id}>{r.arm} &middot; seed {r.seed} &middot; {r.status} &middot; {r.id.slice(-6)}</option>
			{/each}
		</select>
	</div>
{/snippet}

{#snippet answer(view: RunView | undefined)}
	<div class="min-w-0 space-y-1">
		<p class="truncate text-xs text-muted-foreground">
			{#if view}
				{view.status} &middot; {view.tokens.length} tokens &middot; captured {view.counters.captured} / skipped
				{view.counters.skipped} / dropped {view.counters.dropped}
			{:else}
				no run selected
			{/if}
		</p>
		<div class="max-h-28 min-h-9 overflow-y-auto rounded border px-2 py-1 font-mono text-sm leading-6">
			{#if view}
				{#each view.tokens as t (t.generation_index)}
					<span
						class={['rounded px-0.5 whitespace-pre', divergeAt !== null && t.generation_index >= divergeAt && 'bg-amber-500/20']}
						title="#{t.generation_index} id {t.token_id}">{label(t)}</span
					>
				{/each}
			{/if}
		</div>
	</div>
{/snippet}

{#snippet card(view: RunView | undefined)}
	{#if view}
		<ResearchDistributionCard {view} tokens={view.tokens} observations={view.observations} selected={0} />
	{:else}
		<div class="rounded-lg border border-dashed p-6 text-center text-sm text-muted-foreground">Select a run.</div>
	{/if}
{/snippet}

<div class="space-y-3">
	<div class="flex flex-wrap items-center gap-2">
		<h2 class="text-lg font-semibold">Compare runs</h2>
		{#if samePrefix !== null}
			<span
				class={[
					'rounded px-1.5 py-0.5 text-xs font-medium',
					samePrefix ? 'bg-green-600/15 text-green-700 dark:text-green-400' : 'bg-amber-500/15 text-amber-600'
				]}
				title={samePrefix
					? 'Identical prompt and rendered prompt, position 0 captured in both: the cards compare the same context'
					: 'Prompts differ or position 0 is missing in a run: the cards are not a same-context comparison'}
				>{samePrefix ? 'same prefix' : 'different prefix'}</span
			>
		{/if}
	</div>

	<div class="grid items-start gap-x-4 gap-y-2 lg:grid-cols-2">
		{@render picker(0)}
		{@render picker(1)}
		{@render card(viewA)}
		{@render card(viewB)}
	</div>

	{#if viewA && viewB}
		{#if divergeAt === null}
			<div class="rounded border border-green-600/40 bg-green-600/10 p-2 text-sm">
				<span class="font-medium">Same tokens so far:</span> positions are same-prefix comparable.
			</div>
		{:else}
			<div class="rounded border border-amber-500/50 bg-amber-500/10 p-2 text-sm">
				<span class="font-medium">Diverged at generation_index {divergeAt}:</span> later positions are different contexts
				(highlighted). Use the inspector counterfactual for same-prefix effects.
			</div>
		{/if}
	{/if}

	<div class="grid items-start gap-4 lg:grid-cols-2">
		{@render answer(viewA)}
		{@render answer(viewB)}
	</div>

	{#if showCharts}
		<section class="space-y-1">
			<div class="flex flex-wrap items-center gap-1">
				<h3 class="mr-2 text-sm font-medium">Group trajectories</h3>
				{#each Object.keys(METRIC_LABELS) as m (m)}
					<Button size="sm" variant={metric === m ? 'secondary' : 'ghost'} onclick={() => (metric = m as MetricKey)}>{METRIC_LABELS[m as MetricKey]}</Button>
				{/each}
			</div>
			<div class="grid gap-4 lg:grid-cols-2">
				<ResearchTrajectoryChart series={seriesA} {range} />
				<ResearchTrajectoryChart series={seriesB} {range} />
			</div>
		</section>
	{:else if viewA && viewB}
		<p class="text-xs text-muted-foreground">
			Trajectories need at least 3 captured positions in both runs (A has {viewA.observations.length}, B has {viewB.observations.length}).
		</p>
	{/if}
</div>
