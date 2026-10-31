<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { researchStore as rs } from '$lib/stores/research.svelte';
	import { stripEog } from '$lib/utils/research';
	import { untrack } from 'svelte';
	import ResearchBatchMatrix from './ResearchBatchMatrix.svelte';
	import ResearchRunInspector from './ResearchRunInspector.svelte';

	// views are created in event handlers (openRun mutates a map; never inside $derived)
	let view = $derived(rs.selectedRunId ? (rs.views.get(rs.selectedRunId) ?? null) : null);

	// one-line display; the exact text is in the tooltip
	const oneLine = (t: string) => stripEog(t).replace(/\s+/g, ' ');

	function select(id: string) {
		rs.openRun(id);
		rs.selectedRunId = id;
		// keep the matrix on the batch of the opened run
		const b = rs.runs.find((r) => r.id === id)?.batch_id;
		if (b && b !== rs.batch?.id) rs.batchPick = b;
	}

	// open the first cell (first arm, first seed) when nothing from the shown batch is open
	$effect(() => {
		const b = rs.batch;
		const sel = rs.selectedRunId;
		if (!b || (sel && b.runs.some((r) => r.id === sel))) return;
		const first =
			b.runs.find((r) => r.arm === b.arms[0] && r.seed === b.seeds[0]) ?? b.runs[0];
		if (first) untrack(() => select(first.id));
	});

	function reload(id: string) {
		rs.closeRun(id);
		rs.openRun(id);
	}

	function addToCompare(id: string) {
		rs.openRun(id);
		const [a, b] = rs.compareIds;
		rs.compareIds = a === null ? [id, b] : b === null ? [a, id] : [b, id];
	}
</script>

<div class="space-y-4">
	<div class="flex flex-wrap items-center gap-2">
		<h2 class="text-lg font-semibold">Runs</h2>
		<Button size="sm" variant="outline" onclick={() => rs.refreshRuns()}>Refresh</Button>
		{#if rs.runs.some((r) => r.status === 'queued' || r.status === 'running')}
			<span class="text-xs text-muted-foreground">auto-refreshing while runs are active</span>
		{/if}
		{#if rs.compareIds[0] || rs.compareIds[1]}
			<span class="ml-auto text-xs text-muted-foreground">
				compare: {rs.compareIds[0]?.slice(-8) ?? '-'} vs {rs.compareIds[1]?.slice(-8) ?? '-'}
				<Button size="sm" variant="link" class="h-auto p-0 pl-1 text-xs" onclick={() => (rs.tab = 'compare')}>open compare</Button>
			</span>
		{/if}
	</div>

	{#if rs.runs.length === 0}
		<p class="text-sm text-muted-foreground">No runs for this question yet.</p>
	{:else}
		<ResearchBatchMatrix onPick={select} />
	{/if}

	{#if view}
		{#key view}
			<ResearchRunInspector {view} onReload={() => reload(view.id)} />
		{/key}
	{/if}

	{#if rs.runs.length}
		<details class="rounded-lg border" open={!rs.batch}>
			<summary class="cursor-pointer px-3 py-2 text-sm font-medium">All runs ({rs.runs.length})</summary>
			<table class="w-full text-xs">
				<thead class="bg-background text-left text-muted-foreground">
					<tr><th class="p-1">run</th><th>arm</th><th>seed</th><th>status</th><th>tokens</th><th>captured / skipped / dropped</th><th>text</th><th></th></tr>
				</thead>
				<tbody>
					{#each rs.runs as r (r.id)}
						<tr class={['border-t', rs.selectedRunId === r.id && 'bg-accent']}>
							<td class="p-1 font-mono" title={r.id}>{r.id.slice(-8)}</td>
							<td>{r.arm}</td>
							<td>{r.seed}</td>
							<td>
								<Badge variant={r.status === 'completed' ? 'secondary' : r.status === 'failed' ? 'destructive' : 'outline'} title={r.error ?? ''}
									>{r.status}</Badge
								>
							</td>
							<td>{r.n_tokens}</td>
							<td>{r.captured} / {r.skipped} / {r.dropped}</td>
							<td class="max-w-sm truncate font-mono" title={JSON.stringify(r.text ?? '')}>{oneLine(r.text) || '-'}</td>
							<td class="whitespace-nowrap">
								<Button size="sm" variant="ghost" onclick={() => select(r.id)}>Open</Button>
								<Button size="sm" variant="ghost" onclick={() => addToCompare(r.id)}>Compare</Button>
							</td>
						</tr>
					{/each}
				</tbody>
			</table>
		</details>
	{/if}
</div>
