<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { researchStore as rs } from '$lib/stores/research.svelte';
	import ResearchRunInspector from './ResearchRunInspector.svelte';

	// views are created in event handlers (openRun mutates a map; never inside $derived)
	let view = $derived(rs.selectedRunId ? (rs.views.get(rs.selectedRunId) ?? null) : null);

	function select(id: string) {
		rs.openRun(id);
		rs.selectedRunId = id;
	}

	function reopen(id: string) {
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
	<div class="flex items-center gap-2">
		<h2 class="text-lg font-semibold">Run library</h2>
		<Button size="sm" variant="outline" onclick={() => rs.refreshRuns()}>Refresh</Button>
		<span class="text-xs text-muted-foreground">Finished runs are loaded from the server database (works after restart).</span>
	</div>

	<div class="max-h-64 overflow-auto rounded border">
		<table class="w-full text-xs">
			<thead class="sticky top-0 bg-background text-left text-muted-foreground">
				<tr><th class="p-1">run</th><th>arm</th><th>seed</th><th>status</th><th>tokens</th><th>captured / skipped / dropped</th><th>text</th><th></th></tr>
			</thead>
			<tbody>
				{#each rs.runs as r (r.id)}
					<tr class={['border-t', rs.selectedRunId === r.id && 'bg-accent']}>
						<td class="p-1 font-mono">{r.id}</td>
						<td>{r.arm}</td>
						<td>{r.seed}</td>
						<td><Badge variant="outline">{r.status}</Badge></td>
						<td>{r.n_tokens}</td>
						<td>{r.captured} / {r.skipped} / {r.dropped}</td>
						<td class="max-w-xs truncate whitespace-pre" title={r.text}>{r.text}</td>
						<td class="whitespace-nowrap">
							<Button size="sm" variant="ghost" onclick={() => select(r.id)}>Open</Button>
							<Button size="sm" variant="ghost" onclick={() => addToCompare(r.id)}>Compare</Button>
						</td>
					</tr>
				{:else}
					<tr><td colspan="8" class="p-2 text-muted-foreground">No runs for this question yet.</td></tr>
				{/each}
			</tbody>
		</table>
	</div>
	{#if rs.compareIds[0] || rs.compareIds[1]}
		<p class="text-xs">
			Compare selection: {rs.compareIds[0] ?? '-'} vs {rs.compareIds[1] ?? '-'}
			<Button size="sm" variant="link" onclick={() => (rs.tab = 'compare')}>Open compare</Button>
		</p>
	{/if}

	{#if view}
		<div class="flex justify-end">
			<Button size="sm" variant="ghost" onclick={() => reopen(view.id)}>Reload run</Button>
		</div>
		{#key view.id}
			<ResearchRunInspector {view} />
		{/key}
	{/if}
</div>
