<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { Textarea } from '$lib/components/ui/textarea';
	import { ResearchService } from '$lib/services/research.service';
	import { researchStore as rs } from '$lib/stores/research.svelte';
	import type { BatchRequest, CollectionMode } from '$lib/types/research';
	import { SvelteSet } from 'svelte/reactivity';

	const SELECT = 'h-8 rounded-md border border-input bg-transparent px-2 text-sm dark:bg-input/30';

	let designId = $state(rs.designId ?? '');
	let prompt = $state('The capital of France is');
	let chat = $state(true);
	let includeControl = $state(true);
	let chosen = new SvelteSet<string>(rs.interventionId ? [rs.interventionId] : []);
	let seedsText = $state('17, 29');
	let nPredict = $state(64);
	let temperature = $state(0.8);
	let topK = $state(40);
	let topP = $state(0.95);
	let everyN = $state(5);
	let mode = $state<CollectionMode>('raw_watch');
	let discoveryTopK = $state(0);

	let usable = $derived(rs.designs.filter((d) => d.state === 'approved' || d.compiled_id || d.state === 'compiled'));
	let ivs = $derived(rs.interventions.filter((i) => i.design_id === designId));
	let seeds = $derived(
		seedsText
			.split(/[\s,]+/)
			.map(Number)
			.filter((n) => Number.isInteger(n))
	);
	let nArms = $derived((includeControl ? 1 : 0) + ivs.filter((i) => chosen.has(i.id)).length);

	async function launch() {
		if (!rs.question) return;
		const arms: BatchRequest['arms'] = [];
		if (includeControl) arms.push({ name: 'control', intervention_id: null });
		for (const iv of ivs) if (chosen.has(iv.id)) arms.push({ name: `int_${iv.id.slice(-6)}`, intervention_id: iv.id });
		const req: BatchRequest = {
			question_id: rs.question.id,
			design_id: designId,
			arms,
			prompt,
			chat,
			seeds,
			n_predict: nPredict,
			sampling: { temperature, top_k: topK, top_p: topP },
			collection: { every_n: everyN, mode, top_k: discoveryTopK }
		};
		const res = await rs.run(() => ResearchService.createBatch(req));
		if (!res) return;
		await rs.refreshRuns();
		for (const id of res.run_ids) rs.openRun(id);
		rs.selectedRunId = res.run_ids[0] ?? null;
		rs.tab = 'runs';
	}
</script>

<div class="max-w-3xl space-y-4 text-sm">
	<h2 class="text-lg font-semibold">Launch a batch</h2>

	<label class="flex items-center gap-2">
		<span class="w-32 text-muted-foreground">Design</span>
		<select bind:value={designId} class={SELECT}>
			<option value="" disabled>select a compiled/approved design</option>
			{#each usable as d (d.id)}<option value={d.id}>r{d.revision} {d.id} ({d.state})</option>{/each}
		</select>
	</label>

	<label class="block space-y-1">
		<span class="text-muted-foreground">Prompt</span>
		<Textarea bind:value={prompt} class="min-h-12 font-mono" />
	</label>
	<label class="flex items-center gap-2">
		<input type="checkbox" bind:checked={chat} /> Apply chat template (single user message)
	</label>

	<fieldset class="space-y-1 rounded-lg border p-3">
		<legend class="px-1 text-muted-foreground">Arms</legend>
		<label class="flex items-center gap-2"><input type="checkbox" bind:checked={includeControl} /> control (no intervention)</label>
		{#each ivs as iv (iv.id)}
			<label class="flex items-center gap-2">
				<input
					type="checkbox"
					checked={chosen.has(iv.id)}
					onchange={(e) => (e.currentTarget.checked ? chosen.add(iv.id) : chosen.delete(iv.id))}
				/>
				{iv.id} <Badge variant="outline">{iv.state ?? 'draft'}</Badge>
				<span class="text-xs text-muted-foreground">
					{iv.rules.map((r) => `${r.operation === 'exclude' ? 'exclude' : r.delta} ${r.target_group_id}`).join('; ')}
				</span>
			</label>
		{:else}
			<p class="text-xs text-muted-foreground">No interventions for this design.</p>
		{/each}
	</fieldset>

	<div class="grid grid-cols-2 gap-3 md:grid-cols-4">
		<label class="space-y-1"><span class="text-muted-foreground">Seeds</span><Input bind:value={seedsText} class="h-8" /></label>
		<label class="space-y-1"><span class="text-muted-foreground">n_predict</span><Input type="number" bind:value={nPredict} class="h-8" /></label>
		<label class="space-y-1"><span class="text-muted-foreground">temperature</span><Input type="number" step="0.1" bind:value={temperature} class="h-8" /></label>
		<label class="space-y-1"><span class="text-muted-foreground">top_k</span><Input type="number" bind:value={topK} class="h-8" /></label>
		<label class="space-y-1"><span class="text-muted-foreground">top_p</span><Input type="number" step="0.05" bind:value={topP} class="h-8" /></label>
		<label class="space-y-1"><span class="text-muted-foreground">capture every_n</span><Input type="number" min="1" bind:value={everyN} class="h-8" /></label>
		<label class="space-y-1">
			<span class="text-muted-foreground">mode</span>
			<select bind:value={mode} class={[SELECT, 'w-full']}>
				<option value="raw_watch">raw_watch (watch-relative)</option>
				<option value="watch_normalized">watch_normalized (+ full log Z, absolute)</option>
			</select>
		</label>
		<label class="space-y-1"><span class="text-muted-foreground">top-k discovery</span><Input type="number" min="0" bind:value={discoveryTopK} class="h-8" /></label>
	</div>

	<div class="flex items-center gap-3">
		<Button onclick={launch} disabled={!designId || nArms === 0 || seeds.length === 0 || rs.busy}>Launch {nArms * seeds.length} runs</Button>
		<span class="text-xs text-muted-foreground">runs = arms x seeds, executed sequentially on the server</span>
	</div>
</div>
