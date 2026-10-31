<script lang="ts">
	import { Button } from '$lib/components/ui/button';
	import { researchStore as rs, type RunView } from '$lib/stores/research.svelte';
	import type { MetricKey, RunToken } from '$lib/types/research';
	import { b64ToHex, buildSeries, groupColor, isNonUtf8, METRIC_LABELS, SELECT_CLASS } from '$lib/utils/research';
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

	function label(t: RunToken): string {
		return isNonUtf8(t.piece_b64) ? `<${b64ToHex(t.piece_b64)}>` : t.piece;
	}
</script>

{#snippet side(view: RunView | undefined, slot: 0 | 1, series: ReturnType<typeof buildSeries>)}
	<div class="min-w-0 space-y-2">
		<div class="flex h-16 flex-col justify-between">
			<div class="flex items-center gap-2">
				<span class="w-4 text-sm font-semibold">{slot === 0 ? 'A' : 'B'}</span>
				<select class={[SELECT_CLASS, 'min-w-0 flex-1']} value={rs.compareIds[slot] ?? ''} onchange={(e) => pick(slot, e.currentTarget.value)}>
					<option value="">select run {slot === 0 ? 'A' : 'B'}</option>
					{#each rs.runs as r (r.id)}
						<option value={r.id}>{r.arm} &middot; seed {r.seed} &middot; {r.status} &middot; {r.id.slice(-6)}</option>
					{/each}
				</select>
			</div>
			<p class="truncate text-xs text-muted-foreground">
				{#if view}
					{view.status} &middot; {view.tokens.length} tokens &middot; captured {view.counters.captured} / skipped
					{view.counters.skipped} / dropped {view.counters.dropped}
				{:else}
					no run selected
				{/if}
			</p>
		</div>
		<div class="h-28 overflow-y-auto rounded border p-2 font-mono text-sm leading-6">
			{#if view}
				{#each view.tokens as t (t.generation_index)}
					<span
						class={['rounded px-0.5 whitespace-pre', divergeAt !== null && t.generation_index >= divergeAt && 'bg-amber-500/20']}
						title="#{t.generation_index} id {t.token_id}">{label(t)}</span
					>
				{/each}
			{/if}
		</div>
		<ResearchTrajectoryChart {series} {range} />
	</div>
{/snippet}

<div class="space-y-3">
	<div class="flex flex-wrap items-center gap-2">
		<h2 class="text-lg font-semibold">Compare runs</h2>
		{#each Object.keys(METRIC_LABELS) as m (m)}
			<Button size="sm" variant={metric === m ? 'secondary' : 'ghost'} onclick={() => (metric = m as MetricKey)}>{METRIC_LABELS[m as MetricKey]}</Button>
		{/each}
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

	<div class="grid gap-4 xl:grid-cols-2">
		{@render side(viewA, 0, seriesA)}
		{@render side(viewB, 1, seriesB)}
	</div>
</div>
