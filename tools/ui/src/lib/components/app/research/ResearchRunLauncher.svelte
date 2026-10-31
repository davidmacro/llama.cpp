<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { Textarea } from '$lib/components/ui/textarea';
	import { ResearchService } from '$lib/services/research.service';
	import { researchStore as rs } from '$lib/stores/research.svelte';
	import type { BatchRequest, CollectionMode } from '$lib/types/research';
	import { ruleSummary, runTime, SELECT_CLASS } from '$lib/utils/research';
	import { SvelteSet } from 'svelte/reactivity';

	const SELECT = SELECT_CLASS;

	let designId = $state(rs.designId ?? '');
	let prompt = $state('');
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
	let ivs = $derived(rs.interventions.filter((i) => i.design_id === designId && !rs.hiddenIvs.has(i.id)));
	let seeds = $derived(
		seedsText
			.split(/[\s,]+/)
			.map(Number)
			.filter((n) => Number.isInteger(n))
	);
	// arm label = rule summary ("NEGATIVE -4"), made unique with the id suffix
	let armName = $derived.by(() => {
		const out = new Map<string, string>();
		const seen = new Set<string>(['control']);
		for (const iv of ivs) {
			let n = ruleSummary(iv.rules, (id) => rs.groupName(id));
			if (seen.has(n)) n = `${n} (${iv.id.slice(-4)})`;
			seen.add(n);
			out.set(iv.id, n);
		}
		return out;
	});
	let prefilledFrom = $state<string | null>(null);

	// prefill from the latest run of this question (runs once per mount)
	async function prefill() {
		const latest = [...rs.runs].sort((a, b) => (runTime(a) < runTime(b) ? 1 : -1))[0];
		if (!latest) return;
		const m = await ResearchService.getRun(latest.id).catch(() => null);
		if (!m) return;
		if (m.prompt) prompt = m.prompt;
		if (typeof m.chat === 'boolean') chat = m.chat;
		if (m.n_predict) nPredict = m.n_predict;
		const sp = m.sampling as { temperature?: number; top_k?: number; top_p?: number } | undefined;
		if (sp?.temperature !== undefined) temperature = sp.temperature;
		if (sp?.top_k !== undefined) topK = sp.top_k;
		if (sp?.top_p !== undefined) topP = sp.top_p;
		if (m.collection) {
			everyN = m.collection.every_n;
			mode = m.collection.mode;
			discoveryTopK = m.collection.top_k;
		}
		const batchRuns = rs.runs.filter((r) => r.batch_id === m.batch_id);
		const seedSet = [...new Set(batchRuns.map((r) => r.seed))].sort((a, b) => a - b);
		if (seedSet.length) seedsText = seedSet.join(', ');
		if (m.design_id && usable.some((d) => d.id === m.design_id) && !rs.designId) designId = m.design_id;
		const armIvs = batchRuns.map((r) => r.intervention_id).filter((id): id is string => !!id && ivs.some((i) => i.id === id));
		if (batchRuns.length) {
			includeControl = batchRuns.some((r) => !r.intervention_id);
			if (armIvs.length) {
				chosen.clear();
				for (const id of armIvs) chosen.add(id);
			}
		}
		prefilledFrom = m.id;
	}
	void prefill();

	let nArms = $derived((includeControl ? 1 : 0) + ivs.filter((i) => chosen.has(i.id)).length);
	let planArms = $derived([
		...(includeControl ? ['control'] : []),
		...ivs.filter((i) => chosen.has(i.id)).map((i) => armName.get(i.id) ?? i.id)
	]);

	async function launch() {
		if (!rs.question) return;
		const arms: BatchRequest['arms'] = [];
		if (includeControl) arms.push({ name: 'control', intervention_id: null });
		for (const iv of ivs) if (chosen.has(iv.id)) arms.push({ name: armName.get(iv.id) ?? iv.id, intervention_id: iv.id });
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

<div class="grid gap-6 text-sm xl:grid-cols-[minmax(0,48rem)_minmax(16rem,1fr)]">
<div class="min-w-0 space-y-4">
	<div class="flex items-baseline gap-2">
		<h2 class="text-lg font-semibold">Launch a batch</h2>
		{#if prefilledFrom}<span class="text-xs text-muted-foreground" title={prefilledFrom}>prefilled from the latest run</span>{/if}
	</div>

	<label class="flex items-center gap-2">
		<span class="w-32 text-muted-foreground">Design</span>
		<select bind:value={designId} class={SELECT}>
			<option value="" disabled>select a compiled/approved design</option>
			{#each usable as d (d.id)}<option value={d.id}>r{d.revision} {d.id} ({d.state})</option>{/each}
		</select>
	</label>

	<label class="block space-y-1">
		<span class="text-muted-foreground">Prompt</span>
		<Textarea bind:value={prompt} class="min-h-12 font-mono" placeholder="Prompt sent to the model, e.g. a question that ends where the answer token starts" />
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
				<span class="font-medium">{armName.get(iv.id)}</span> <Badge variant="outline">{iv.state ?? 'draft'}</Badge>
				<span class="text-[10px] text-muted-foreground">{iv.id}</span>
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
		<Button onclick={launch} disabled={!designId || !prompt.trim() || nArms === 0 || seeds.length === 0 || rs.busy}>Launch {nArms * seeds.length} runs</Button>
		<span class="text-xs text-muted-foreground">runs = arms x seeds, executed sequentially on the server</span>
	</div>
</div>

<aside class="min-w-0 space-y-2 self-start rounded-lg border p-3">
	<h3 class="font-medium">Run plan <span class="text-xs font-normal text-muted-foreground">{nArms} arms &times; {seeds.length} seeds = {nArms * seeds.length} runs</span></h3>
	{#if planArms.length && seeds.length}
		<table class="w-full text-xs">
			<thead class="text-left text-muted-foreground">
				<tr><th class="pb-1 font-medium">arm</th><th class="pb-1 font-medium">seeds</th></tr>
			</thead>
			<tbody>
				{#each planArms as a (a)}
					<tr class="border-t">
						<td class="max-w-48 truncate py-1 pr-2 font-medium" title={a}>{a}</td>
						<td class="py-1 font-mono">{seeds.join(', ')}</td>
					</tr>
				{/each}
			</tbody>
		</table>
	{:else}
		<p class="text-xs text-muted-foreground">Pick at least one arm and one seed.</p>
	{/if}
	<dl class="grid grid-cols-[auto_1fr] gap-x-3 gap-y-0.5 border-t pt-2 text-xs">
		<dt class="text-muted-foreground">tokens</dt><dd>up to {nPredict} per run</dd>
		<dt class="text-muted-foreground">sampling</dt><dd>T {temperature} &middot; top_k {topK} &middot; top_p {topP}</dd>
		<dt class="text-muted-foreground">capture</dt><dd>every {everyN} &middot; {mode}{#if discoveryTopK}&nbsp;&middot; top-{discoveryTopK}{/if}</dd>
		<dt class="text-muted-foreground">prompt</dt><dd class="truncate font-mono" title={prompt}>{prompt || '-'}</dd>
	</dl>
</aside>
</div>
