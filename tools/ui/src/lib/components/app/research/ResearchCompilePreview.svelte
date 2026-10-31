<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import type { CompiledPlan, DesignGroup } from '$lib/types/research';
	import { isGenericPiece, visiblePiece } from '$lib/utils/research';
	import ResearchPiece from './ResearchPiece.svelte';

	interface Props {
		groups: DesignGroup[];
		onUseFirst?: (groupId: string, tokenId: number) => void;
		plan: CompiledPlan;
	}

	let { groups, onUseFirst, plan }: Props = $props();

	let byId = $derived(new Map(groups.map((g) => [g.id, g])));
	let overlapIds = $derived(new Set(plan.overlaps.map((o) => o.token_id)));

	const nameOf = (id: string) => byId.get(id)?.name ?? plan.groups.find((g) => g.id === id)?.name ?? id;
	const colorOf = (id: string) => byId.get(id)?.color ?? '#888';
	const isApprox = (gid: string, tid: number) => byId.get(gid)?.approx_ids?.includes(tid) ?? false;

	// a resolved member (not a multi-token split) whose visible text is 1-2 chars
	const genericMember = (status: string, piece: string | undefined) => status !== 'multi_token' && isGenericPiece(piece);

	function statusClass(s: string): string {
		if (s === 'accepted_single_token') return 'text-green-600';
		if (s === 'multi_token' || s === 'ambiguous_duplicate_piece') return 'text-amber-600';
		return 'text-destructive';
	}

	function statusLabel(s: string): string {
		return s === 'accepted_single_token' ? 'ok' : s.replace(/_/g, ' ');
	}
</script>

<section class="space-y-2 rounded-lg border p-3 text-sm">
	<div class="flex flex-wrap items-center gap-2">
		<h3 class="font-medium">Compiled plan</h3>
		<Badge variant={plan.ok ? 'default' : 'destructive'}>{plan.ok ? 'ok' : 'has errors'}</Badge>
		<span class="text-xs text-muted-foreground" title={plan.fingerprint}>
			{plan.compiled_id ? 'stored' : 'preview'} &middot; watch {plan.watch_ids.length} ids
		</span>
	</div>

	{#if plan.warnings.length}
		<ul class="list-disc pl-5 text-xs text-amber-600">
			{#each plan.warnings as w, i (i)}<li>{w}</li>{/each}
		</ul>
	{/if}

	<table class="w-full text-xs">
		<thead class="text-left text-muted-foreground">
			<tr><th class="pr-2">group</th><th class="pr-2">value</th><th class="pr-2">status</th><th>resolved</th></tr>
		</thead>
		<tbody>
			{#each plan.candidates as c, i (i)}
				<tr class="border-t align-top">
					<td class="py-0.5 pr-2 font-medium whitespace-nowrap" style:color={colorOf(c.group_id)} title="id {c.group_id}"
						>{nameOf(c.group_id)}</td
					>
					<td class="py-0.5 pr-2" title={c.selector_kind}>
						{#if typeof c.value === 'string'}<ResearchPiece piece={c.value} />{:else}#{c.value}{/if}
					</td>
					<td class={['py-0.5 pr-2 whitespace-nowrap', statusClass(c.status)]}>{statusLabel(c.status)}</td>
					<td class="py-0.5">
						<div class="flex flex-wrap items-center gap-1">
							{#each c.token_ids as id, j (j)}
								{@const generic = genericMember(c.status, c.pieces[j])}
								<span class={['rounded border px-1', (overlapIds.has(id) || generic) && 'border-amber-500']}>
									{id}
									{#if c.pieces[j] !== undefined}<ResearchPiece piece={c.pieces[j]} />{/if}
									{#if generic}<span class="text-amber-600" title="'{visiblePiece(c.pieces[j] ?? '')}' is so short that many unrelated words start with it">generic prefix</span>{/if}
									{#if isApprox(c.group_id, id)}<span class="text-amber-600">approx.</span>{/if}
								</span>
							{/each}
							{#if c.status === 'multi_token' && c.token_ids.length && onUseFirst}
								{@const first = c.token_ids[0]}
								{#if isGenericPiece(c.pieces[0])}
									<span class="text-amber-600" title="First piece '{visiblePiece(c.pieces[0] ?? '')}' is so short that many unrelated words start with it"
										>generic prefix</span
									>
								{/if}
								{#if isApprox(c.group_id, first)}
									<span class="text-muted-foreground">first token added</span>
								{:else}
									<Button
										size="sm"
										variant="outline"
										class="h-5 px-1.5 text-[10px]"
										title="Add token {first} as an approximate member of {nameOf(c.group_id)}"
										onclick={() => onUseFirst(c.group_id, first)}>use first token (approx.)</Button
									>
								{/if}
							{/if}
						</div>
					</td>
				</tr>
			{/each}
		</tbody>
	</table>

	<div class="space-y-0.5 border-t pt-2">
		{#each plan.groups as g (g.id)}
			<div class="text-xs">
				<span class="font-medium" style:color={colorOf(g.id)}>{nameOf(g.id)}</span>
				<span class="text-muted-foreground">{g.role}, {g.members.length} ids:</span>
				<code>
					{#each g.members as m, i (m)}{i ? ', ' : ''}{m}{#if isApprox(g.id, m)}<span class="text-amber-600">~</span>{/if}{:else}EMPTY{/each}
				</code>
				{#each g.warnings as w, i (i)}<span class="ml-2 text-amber-600">{w}</span>{/each}
			</div>
		{/each}
		{#if groups.some((g) => g.approx_ids?.length)}
			<p class="text-[10px] text-amber-600">~ approx.: first token of a multi-token word, not the whole word.</p>
		{/if}
	</div>

	{#if plan.overlaps.length}
		<div class="text-xs text-amber-600">
			Overlaps:
			{#each plan.overlaps as o (o.token_id)}
				<span class="ml-2">{o.token_id} -> {o.group_ids.map(nameOf).join(', ')}</span>
			{/each}
		</div>
	{/if}
</section>
