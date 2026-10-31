<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import type { CompiledPlan, DesignGroup } from '$lib/types/research';
	import ResearchPiece from './ResearchPiece.svelte';

	interface Props {
		groups: DesignGroup[];
		plan: CompiledPlan;
	}

	let { groups, plan }: Props = $props();

	let colorOf = $derived(new Map(groups.map((g) => [g.id, g.color ?? '#888'])));
	let overlapIds = $derived(new Set(plan.overlaps.map((o) => o.token_id)));

	function statusClass(s: string): string {
		if (s === 'accepted_single_token') return 'text-green-600';
		if (s === 'multi_token' || s === 'ambiguous_duplicate_piece') return 'text-amber-600';
		return 'text-destructive';
	}
</script>

<section class="space-y-3 rounded-lg border p-3 text-sm">
	<div class="flex flex-wrap items-center gap-2">
		<h3 class="font-medium">Compiled plan</h3>
		<Badge variant={plan.ok ? 'default' : 'destructive'}>{plan.ok ? 'ok' : 'has errors'}</Badge>
		<span class="text-xs text-muted-foreground">
			{plan.compiled_id ? `stored ${plan.compiled_id}` : 'preview (not stored)'} · watch {plan.watch_ids.length} ids
		</span>
		<code class="max-w-full truncate text-xs text-muted-foreground" title={plan.fingerprint}>{plan.fingerprint}</code>
	</div>

	{#if plan.warnings.length}
		<ul class="list-disc pl-5 text-xs text-amber-600">
			{#each plan.warnings as w, i (i)}<li>{w}</li>{/each}
		</ul>
	{/if}

	<div class="overflow-x-auto">
		<table class="w-full text-xs">
			<thead class="text-left text-muted-foreground">
				<tr><th class="pr-2">group</th><th class="pr-2">selector</th><th class="pr-2">value</th><th class="pr-2">status</th><th>resolved</th></tr>
			</thead>
			<tbody>
				{#each plan.candidates as c, i (i)}
					<tr class="border-t align-top">
						<td class="pr-2" style:color={colorOf.get(c.group_id)}>{c.group_id}</td>
						<td class="pr-2">{c.selector_kind}</td>
						<td class="pr-2">
							{#if typeof c.value === 'string'}<ResearchPiece piece={c.value} />{:else}{c.value}{/if}
						</td>
						<td class={['pr-2 font-medium', statusClass(c.status)]}>{c.status}</td>
						<td>
							<div class="flex flex-wrap gap-1">
								{#each c.token_ids as id, j (j)}
									<span class={['rounded border px-1', overlapIds.has(id) && 'border-amber-500']}>
										{id}
										{#if c.pieces[j] !== undefined}<ResearchPiece piece={c.pieces[j]} />{/if}
									</span>
								{/each}
							</div>
						</td>
					</tr>
				{/each}
			</tbody>
		</table>
	</div>

	<div class="space-y-1">
		<h4 class="text-xs font-medium text-muted-foreground uppercase">Group members</h4>
		{#each plan.groups as g (g.id)}
			<div class="text-xs">
				<span class="font-medium" style:color={colorOf.get(g.id)}>{g.name}</span>
				({g.role}, {g.members.length} ids): <code>{g.members.join(', ') || 'EMPTY'}</code>
				{#each g.warnings as w, i (i)}<span class="ml-2 text-amber-600">{w}</span>{/each}
			</div>
		{/each}
	</div>

	{#if plan.overlaps.length}
		<div class="text-xs text-amber-600">
			Overlaps (token in several groups):
			{#each plan.overlaps as o (o.token_id)}
				<span class="ml-2">{o.token_id} -> {o.group_ids.join(', ')}</span>
			{/each}
		</div>
	{/if}
</section>
