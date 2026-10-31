<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { researchStore as rs } from '$lib/stores/research.svelte';
	import { fmt, ruleSummary, SELECT_CLASS, shiftedMass } from '$lib/utils/research';
	import ResearchPiece from './ResearchPiece.svelte';

	const SELECT = SELECT_CLASS;
	const nameOf = (id: string) => rs.groupName(id);

	let groups = $derived(rs.draft.groups);
	let showHidden = $state(false);
	let ivAll = $derived(rs.interventions.filter((i) => i.design_id === rs.designId));
	let ivForDesign = $derived(showHidden ? ivAll : ivAll.filter((i) => !rs.hiddenIvs.has(i.id)));
	let nHidden = $derived(ivAll.filter((i) => rs.hiddenIvs.has(i.id)).length);
	// rule id -> "GROUP -3" for the effective bias table
	let ruleLabel = $derived.by(() => {
		const iv = rs.interventions.find((i) => i.id === rs.interventionId);
		const rules = iv?.rules ?? rs.ruleDraft;
		return new Map(rules.map((r) => [r.id, ruleSummary([{ ...r, enabled: true }], nameOf)]));
	});

	// synthetic formula preview (pure client math)
	let pDemo = $state(0.6);
	let dDemo = $state(-3);
	let pShifted = $derived(shiftedMass(Math.min(Math.max(pDemo, 0), 1), dDemo));

	function addRule() {
		let n = rs.ruleDraft.length + 1;
		while (rs.ruleDraft.some((r) => r.id === `r${n}`)) n++;
		rs.ruleDraft.push({
			id: `r${n}`,
			target_group_id: groups[0]?.id ?? '',
			operation: 'add_logit_bias',
			delta: -3,
			enabled: true
		});
	}
</script>

<div class="grid gap-6 xl:grid-cols-2">
	<section class="space-y-3">
		<h2 class="text-lg font-semibold">
			New intervention
			{#if rs.design}
				<span class="text-sm font-normal text-muted-foreground"
					>for design <span class="font-medium text-foreground">r{rs.design.revision ?? '?'}</span> ({rs.design.state})</span
				>
			{/if}
		</h2>
		{#if !rs.designId}
			<p class="text-sm text-amber-600">Save (and ideally approve) a design revision first; interventions reference a design.</p>
		{:else}
			<p class="text-xs text-muted-foreground">
				Each rule adds <i>delta</i> to the logits of every member of a group
				(applied through the sampler logit_bias stage); exclude sets them to -inf and wins.
			</p>
		{/if}

		<table class="w-full text-sm">
			<thead class="text-left text-xs text-muted-foreground">
				<tr><th>id</th><th>group</th><th>operation</th><th>delta</th><th>enabled</th><th></th></tr>
			</thead>
			<tbody>
				{#each rs.ruleDraft as r, i (r.id)}
					<tr class="border-t">
						<td class="pr-2 font-mono text-xs">{r.id}</td>
						<td class="pr-2">
							<select bind:value={r.target_group_id} class={SELECT}>
								{#each groups as g (g.id)}<option value={g.id}>{g.name}</option>{/each}
							</select>
						</td>
						<td class="pr-2">
							<select bind:value={r.operation} class={SELECT}>
								<option value="add_logit_bias">add_logit_bias</option>
								<option value="exclude">exclude</option>
							</select>
						</td>
						<td class="pr-2">
							<Input type="number" step="0.5" bind:value={r.delta} class="h-8 w-24" disabled={r.operation === 'exclude'} />
						</td>
						<td class="pr-2"><input type="checkbox" bind:checked={r.enabled} /></td>
						<td><Button size="sm" variant="ghost" onclick={() => rs.ruleDraft.splice(i, 1)}>x</Button></td>
					</tr>
				{:else}
					<tr><td colspan="6" class="py-1 text-xs text-muted-foreground">Empty draft: add a rule, or duplicate a saved intervention below.</td></tr>
				{/each}
			</tbody>
		</table>
		<div class="flex flex-wrap gap-2">
			<Button size="sm" variant="outline" onclick={addRule} disabled={groups.length === 0}>Add rule</Button>
			<Button size="sm" onclick={() => rs.saveIntervention(false)} disabled={!rs.designId || rs.ruleDraft.length === 0}>Save</Button>
			<Button size="sm" variant="secondary" onclick={() => rs.saveIntervention(true)} disabled={!rs.designId || rs.ruleDraft.length === 0}
				>Save + approve</Button
			>
		</div>

		<div class="space-y-1">
			<div class="flex items-center gap-2">
				<h3 class="text-sm font-medium">Saved interventions for design r{rs.design?.revision ?? '?'}</h3>
				{#if nHidden}
					<label class="ml-auto flex items-center gap-1 text-xs text-muted-foreground">
						<input type="checkbox" bind:checked={showHidden} /> show hidden ({nHidden})
					</label>
				{/if}
			</div>
			{#each ivForDesign as iv (iv.id)}
				{@const hidden = rs.hiddenIvs.has(iv.id)}
				<div class={['flex items-center gap-1 rounded', rs.interventionId === iv.id && 'bg-accent', hidden && 'opacity-50']}>
					<Button size="sm" variant="ghost" class="min-w-0 flex-1 justify-start" title="Show effective bias" onclick={() => rs.selectIntervention(iv.id)}>
						<span class="truncate">{ruleSummary(iv.rules, nameOf)}</span> <Badge variant="outline">{iv.state ?? 'draft'}</Badge>
						<span class="text-[10px] text-muted-foreground">{iv.id}</span>
					</Button>
					<Button size="sm" variant="ghost" class="h-7 px-2 text-xs" title="Copy these rules into the editor as a new draft" onclick={() => rs.duplicateIntervention(iv.id)}
						>duplicate</Button
					>
					<Button size="sm" variant="ghost" class="h-7 px-2 text-xs" title="Hide in this browser only (not deleted)" onclick={() => rs.toggleHidden(iv.id)}
						>{hidden ? 'unhide' : 'hide'}</Button
					>
				</div>
			{:else}
				<p class="text-xs text-muted-foreground">none</p>
			{/each}
		</div>

		<div class="space-y-2 rounded-lg border border-dashed p-3 text-sm">
			<h3 class="font-medium">Formula preview <Badge variant="outline">synthetic</Badge></h3>
			<p class="font-mono text-xs">p' = p e^d / (1 - p + p e^d)</p>
			<p class="text-xs text-muted-foreground">
				Mass of a group after adding d to all its members' logits, given its pre-intervention
				probability p in the same context. Not data from any run.
			</p>
			<div class="flex flex-wrap items-center gap-2">
				<label class="flex items-center gap-1">p <Input type="number" step="0.05" min="0" max="1" bind:value={pDemo} class="h-8 w-24" /></label>
				<label class="flex items-center gap-1">d <Input type="number" step="0.5" bind:value={dDemo} class="h-8 w-24" /></label>
				<span>p' = <b>{fmt(pShifted, 4)}</b></span>
			</div>
		</div>
	</section>

	<section class="space-y-3">
		<h2 class="text-lg font-semibold">
			Effective bias
			{#if rs.interventionId && rs.effectiveBias}
				{@const sel = rs.interventions.find((i) => i.id === rs.interventionId)}
				<span class="text-sm font-normal text-muted-foreground">&middot; {sel ? ruleSummary(sel.rules, nameOf) : rs.interventionId}</span>
			{/if}
		</h2>
		{#if !rs.effectiveBias}
			<p class="text-sm text-muted-foreground">Save or select an intervention to see the per-token bias map.</p>
		{:else}
			{#if rs.effectiveBias.warnings.length}
				<ul class="list-disc pl-5 text-xs text-amber-600">
					{#each rs.effectiveBias.warnings as w, i (i)}<li>{w}</li>{/each}
				</ul>
			{/if}
			<table class="w-full text-xs">
				<thead class="text-left text-muted-foreground">
					<tr><th>token</th><th>piece</th><th>delta</th><th>rules</th></tr>
				</thead>
				<tbody>
					{#each rs.effectiveBias.entries as e (e.token_id)}
						<tr class={['border-t', e.rules.length > 1 && 'bg-amber-500/10']}>
							<td class="pr-2 font-mono">{e.token_id}</td>
							<td class="pr-2"><ResearchPiece piece={e.piece} /></td>
							<td class="pr-2 font-mono">{e.excluded ? 'exclude (-inf)' : fmt(e.delta, 2)}</td>
							<td>
								{e.rules.map((r) => ruleLabel.get(r) ?? r).join(', ')}
								{#if e.rules.length > 1}<span class="text-amber-600">overlap: additive</span>{/if}
							</td>
						</tr>
					{:else}
						<tr><td colspan="4" class="text-muted-foreground">no tokens affected</td></tr>
					{/each}
				</tbody>
			</table>
		{/if}
	</section>
</div>
