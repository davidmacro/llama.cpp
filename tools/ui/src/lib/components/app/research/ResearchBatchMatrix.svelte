<script lang="ts">
	import { researchStore as rs, type BatchInfo } from '$lib/stores/research.svelte';
	import type { RunSummary } from '$lib/types/research';
	import { fmtMass, groupColor, SELECT_CLASS, stripEog } from '$lib/utils/research';

	interface Props {
		/** Read-only summary (question tab): no batch selector. */
		compact?: boolean;
		onPick: (runId: string) => void;
	}

	let { compact = false, onPick }: Props = $props();

	let batch = $derived(rs.batch);

	// run per (arm, seed); a batch has at most one run per cell
	let cells = $derived.by(() => {
		const m = new Map<string, RunSummary>();
		for (const r of batch?.runs ?? []) m.set(`${r.arm}\u0000${r.seed}`, r);
		return m;
	});

	let colorByName = $derived(new Map(rs.draft.groups.map((g, i) => [g.id, g.color ?? groupColor(i)])));

	$effect(() => {
		if (batch) rs.ensureDigests(batch.runs);
	});

	function batchLabel(b: BatchInfo): string {
		const d = b.time ? new Date(b.time) : null;
		const t = d && !Number.isNaN(d.getTime()) ? d.toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' }) : b.id;
		return `${t} \u00b7 ${b.arms.join(' / ')}`;
	}
</script>

{#if batch}
	<section class="space-y-2">
		<div class="flex flex-wrap items-center gap-2 text-sm">
			<h3 class="font-medium">{compact ? 'Latest batch' : 'Batch results'}</h3>
			{#if compact}
				<span class="text-xs text-muted-foreground">{batchLabel(batch)}</span>
			{:else}
				<select class={SELECT_CLASS} value={batch.id} onchange={(e) => (rs.batchPick = e.currentTarget.value)}>
					{#each rs.batches as b (b.id)}<option value={b.id}>{batchLabel(b)}</option>{/each}
				</select>
			{/if}
			<span class="text-xs text-muted-foreground">answer &middot; top watched group (post_intervention) at the first captured position</span>
		</div>
		<div class="overflow-x-auto">
			<table class="w-full table-fixed border-collapse text-sm">
				<thead>
					<tr class="text-left text-xs text-muted-foreground">
						<th class="w-44 p-1 font-medium">arm \ seed</th>
						{#each batch.seeds as seed (seed)}<th class="p-1 font-medium">seed {seed}</th>{/each}
					</tr>
				</thead>
				<tbody>
					{#each batch.arms as arm (arm)}
						<tr class="border-t align-top">
							<th class="truncate p-1 text-left font-medium" title={arm}>{arm}</th>
							{#each batch.seeds as seed (seed)}
								{@const r = cells.get(`${arm}\u0000${seed}`)}
								{@const dg = r ? rs.digests.get(r.id) : undefined}
								<td class="p-1">
									{#if r}
										<button
											class={[
												'w-full rounded border px-2 text-left hover:bg-muted',
												compact ? 'py-1' : 'py-1.5',
												rs.selectedRunId === r.id && !compact && 'bg-accent ring-1 ring-primary/50'
											]}
											title="{r.id} ({r.status}): {JSON.stringify(r.text ?? '')}"
											onclick={() => onPick(r.id)}
										>
											<div class="truncate font-mono">
												{#if r.status === 'completed'}{stripEog(r.text) || '(empty)'}{:else}<span class="text-muted-foreground">{r.status}</span>{/if}
											</div>
											<div class="truncate text-xs text-muted-foreground">
												{#if dg?.groupName}
													<span class="font-medium" style:color={dg.groupId ? colorByName.get(dg.groupId) : undefined}>{dg.groupName}</span>
													{fmtMass(dg.mass)}{dg.massKind === 'rel' ? ' rel' : ''}
												{:else if dg}no captured position{:else}&nbsp;{/if}
											</div>
										</button>
									{:else}
										<span class="text-xs text-muted-foreground">-</span>
									{/if}
								</td>
							{/each}
						</tr>
					{/each}
				</tbody>
			</table>
		</div>
	</section>
{/if}
