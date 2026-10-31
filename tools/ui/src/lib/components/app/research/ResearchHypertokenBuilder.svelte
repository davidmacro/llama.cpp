<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { cleanSpec, ResearchService } from '$lib/services/research.service';
	import { errMsg, researchStore as rs } from '$lib/stores/research.svelte';
	import type { DesignGroup, DesignSpec, TokenInfo } from '$lib/types/research';
	import { isGenericPiece, SELECT_CLASS } from '$lib/utils/research';
	import ResearchCompilePreview from './ResearchCompilePreview.svelte';
	import ResearchPiece from './ResearchPiece.svelte';

	let activeGroupIdx = $state(0);
	let addText = $state<Record<string, string>>({});

	let vocabQuery = $state('');
	let vocabMode = $state<'substring' | 'exact' | 'prefix'>('substring');
	let vocabResults = $state.raw<TokenInfo[]>([]);
	let vocabError = $state<string | null>(null);

	let tokText = $state('');
	let tokResults = $state.raw<TokenInfo[]>([]);

	let design = $derived(rs.design);
	let immutable = $derived(design?.state === 'approved');
	// unsaved = draft differs from the loaded revision (UI-only fields ignored)
	let unsaved = $derived.by(() => {
		const draft = JSON.stringify(cleanSpec($state.snapshot(rs.draft) as DesignSpec));
		if (!design) return rs.draft.groups.length > 0;
		return draft !== JSON.stringify(cleanSpec(design.spec));
	});

	// pieces for explicit token ids shown as chips
	$effect(() => {
		const ids: number[] = [];
		for (const g of rs.draft.groups)
			for (const s of g.selectors) if (s.kind === 'token_ids') ids.push(...s.ids);
		rs.ensureTokens(ids);
	});

	const keyOf = (g: DesignGroup) => g.uid ?? g.id;

	function addGroup() {
		const n = rs.draft.groups.length;
		rs.addGroup(`GROUP_${n + 1}`, n === 0 ? 'target' : 'competitor');
		activeGroupIdx = n;
	}

	function removeGroup(i: number) {
		rs.draft.groups.splice(i, 1);
		activeGroupIdx = Math.max(0, Math.min(activeGroupIdx, rs.draft.groups.length - 1));
	}

	function addTokenId(g: DesignGroup, id: number) {
		const sel = g.selectors.find((s) => s.kind === 'token_ids');
		if (!sel) g.selectors.push({ kind: 'token_ids', ids: [id] });
		else if (sel.kind === 'token_ids' && !sel.ids.includes(id)) sel.ids.push(id);
	}

	function addVariant(g: DesignGroup, text: string) {
		const sel = g.selectors.find((s) => s.kind === 'variant_list');
		if (!sel) g.selectors.push({ kind: 'variant_list', texts: [text] });
		else if (sel.kind === 'variant_list' && !sel.texts.includes(text)) sel.texts.push(text);
	}

	/** "#123" adds a token id; anything else a variant (leading space kept). */
	function submitAdd(g: DesignGroup) {
		const k = keyOf(g);
		const raw = addText[k] ?? '';
		const m = /^#(\d+)$/.exec(raw.trim());
		if (m) addTokenId(g, Number(m[1]));
		else if (raw.trim()) addVariant(g, raw.replace(/\s+$/, ''));
		addText[k] = '';
	}

	function removeTokenId(g: DesignGroup, si: number, id: number) {
		const s = g.selectors[si];
		if (s.kind !== 'token_ids') return;
		s.ids = s.ids.filter((x) => x !== id);
		if (s.ids.length === 0) g.selectors.splice(si, 1);
		if (g.approx_ids) g.approx_ids = g.approx_ids.filter((x) => x !== id);
	}

	function removeVariant(g: DesignGroup, si: number, ti: number) {
		const s = g.selectors[si];
		if (s.kind !== 'variant_list') return;
		s.texts.splice(ti, 1);
		if (s.texts.length === 0) g.selectors.splice(si, 1);
	}

	function parseIds(text: string): number[] {
		return text
			.split(/[\s,]+/)
			.map((x) => Number(x))
			.filter((x) => Number.isInteger(x) && x >= 0);
	}

	async function searchVocab() {
		vocabError = null;
		try {
			vocabResults = (await ResearchService.vocab(vocabQuery, vocabMode, 100)).tokens;
		} catch (e) {
			vocabError = errMsg(e);
		}
	}

	async function tokenize() {
		vocabError = null;
		try {
			tokResults = (await ResearchService.tokenizePreview(tokText)).tokens;
		} catch (e) {
			vocabError = errMsg(e);
		}
	}
</script>

<div class="space-y-3">
	<div class="flex flex-wrap items-center gap-2 text-sm">
		<h2 class="mr-2 text-lg font-semibold">Hypertoken groups</h2>
		<span class="text-muted-foreground">Revisions:</span>
		{#each rs.designs as d (d.id)}
			<Button size="sm" variant={rs.designId === d.id ? 'secondary' : 'ghost'} class="h-7 px-2" onclick={() => rs.loadDesignIntoDraft(d)}>
				r{d.revision ?? '?'} <Badge variant={d.state === 'approved' ? 'default' : 'outline'}>{d.state}</Badge>
			</Button>
		{:else}
			<span class="text-xs text-muted-foreground">none saved yet</span>
		{/each}
		{#if unsaved}
			<Badge variant="outline" class="border-amber-500/60 text-amber-600" title="The draft differs from the selected revision; Save revision creates a new one">unsaved changes</Badge>
		{/if}
		<div class="ml-auto flex flex-wrap gap-2">
			<Button size="sm" variant="outline" onclick={() => rs.compilePreview()} disabled={rs.draft.groups.length === 0}
				>Compile preview</Button
			>
			<Button size="sm" onclick={() => rs.saveDesign()} disabled={rs.draft.groups.length === 0}>Save revision</Button>
			<Button size="sm" variant="outline" onclick={() => rs.compileOnly()} disabled={!rs.designId}>Compile saved</Button>
			<Button size="sm" variant="secondary" onclick={() => rs.compileAndApprove()} disabled={!rs.designId || immutable}
				>Compile + approve</Button
			>
		</div>
	</div>
	{#if immutable}
		<p class="text-xs text-amber-600">Approved revision (immutable): saving creates a new draft revision.</p>
	{/if}

	<div class="grid gap-4 xl:grid-cols-2">
		<div class="min-w-0 space-y-2">
			{#each rs.draft.groups as g, gi (keyOf(g))}
				<div
					class={['space-y-1.5 rounded-lg border p-2', activeGroupIdx === gi && 'ring-2 ring-ring/40']}
					style:border-left={`4px solid ${g.color ?? '#888'}`}
					onfocusin={() => (activeGroupIdx = gi)}
				>
					<div class="flex items-center gap-2">
						<input type="color" bind:value={g.color} class="h-7 w-7 shrink-0 cursor-pointer rounded border" title="Group color (UI only)" />
						<Input value={g.name} oninput={(e) => rs.renameGroup(g, e.currentTarget.value)} class="h-8 min-w-0 flex-1 font-medium" />
						<select bind:value={g.role} class={SELECT_CLASS}>
							<option value="target">target</option>
							<option value="competitor">competitor</option>
							<option value="control">control</option>
						</select>
						<code class="hidden max-w-32 truncate text-[10px] text-muted-foreground 2xl:inline" title="group id{rs.designId ? '' : ' (follows the name until saved)'}">{g.id}</code>
						<!-- always rendered so the name field keeps its width -->
						<Badge variant="outline" class={['text-[10px]', activeGroupIdx !== gi && 'invisible']} title="Vocabulary clicks add to this group">vocab target</Badge>
						<Button size="sm" variant="ghost" class="h-7 px-2" title="Remove group" onclick={() => removeGroup(gi)}>x</Button>
					</div>

					<div class="flex flex-wrap items-center gap-1">
						{#each g.selectors as s, si (si)}
							{#if s.kind === 'variant_list'}
								{#each s.texts as t, ti (ti)}
									{#if t.length}
										<span class={['inline-flex items-center gap-1 rounded border pl-0.5', isGenericPiece(t) && 'border-amber-500/60']}>
											<ResearchPiece piece={t} />
											{#if isGenericPiece(t)}<span class="text-[10px] text-amber-600" title="So short that many unrelated words start with it">generic prefix</span>{/if}
											<button class="px-1 text-xs text-muted-foreground hover:text-foreground" title="remove" onclick={() => removeVariant(g, si, ti)}>x</button>
										</span>
									{/if}
								{/each}
							{:else if s.kind === 'exact_token_piece'}
								<span class="inline-flex items-center rounded border pl-1 text-[10px] text-muted-foreground">
									exact <ResearchPiece piece={s.text} class="ml-1" />
									{#if isGenericPiece(s.text)}<span class="ml-1 text-amber-600" title="So short that many unrelated words start with it">generic prefix</span>{/if}
									<button class="px-1 text-xs hover:text-foreground" title="remove" onclick={() => g.selectors.splice(si, 1)}>x</button>
								</span>
							{:else}
								{#each s.ids as id (id)}
									{@const info = rs.tokenCache.get(id)}
									{@const approx = g.approx_ids?.includes(id)}
									{@const generic = isGenericPiece(info?.piece)}
									<span
										class={['inline-flex items-center gap-1 rounded border pl-1 text-xs', (approx || generic) && 'border-amber-500/60']}
										title={approx ? 'approximate: first token of a multi-token word' : `token id ${id}`}
									>
										<span class="font-mono text-muted-foreground">#{id}</span>
										{#if info}<ResearchPiece piece={info.piece} b64={info.piece_b64} />{/if}
										{#if approx}<span class="text-[10px] text-amber-600">approx.</span>{/if}
										{#if generic}<span class="text-[10px] text-amber-600" title="So short that many unrelated words start with it">generic prefix</span>{/if}
										<button class="px-1 text-muted-foreground hover:text-foreground" title="remove" onclick={() => removeTokenId(g, si, id)}>x</button>
									</span>
								{/each}
							{/if}
						{/each}
						<form
							class="inline-flex"
							onsubmit={(e) => {
								e.preventDefault();
								submitAdd(g);
							}}
						>
							<input
								class="h-7 w-36 rounded-md border border-input bg-transparent px-2 font-mono text-xs dark:bg-input/30"
								placeholder="+ variant or #id"
								title="Enter adds; a leading space is kept; #123 adds token id 123"
								value={addText[keyOf(g)] ?? ''}
								oninput={(e) => (addText[keyOf(g)] = e.currentTarget.value)}
							/>
						</form>
					</div>
				</div>
			{/each}

			<div class="flex flex-wrap items-center gap-2">
				<Button size="sm" variant="secondary" onclick={addGroup}>+ Group</Button>
				<label class="ml-auto flex items-center gap-2 text-xs">
					<span class="text-muted-foreground">Extra watched ids</span>
					<Input
						class="h-8 w-40 font-mono"
						value={rs.draft.watch_extra_token_ids.join(', ')}
						onchange={(e) => (rs.draft.watch_extra_token_ids = parseIds(e.currentTarget.value))}
					/>
				</label>
			</div>

			<details class="rounded-lg border p-2 text-sm">
				<summary class="cursor-pointer font-medium">Vocabulary search &amp; tokenize</summary>
				<div class="mt-2 space-y-2">
					<form
						class="flex gap-2"
						onsubmit={(e) => {
							e.preventDefault();
							void searchVocab();
						}}
					>
						<Input bind:value={vocabQuery} placeholder="search vocabulary, e.g. Paris" class="h-8" />
						<select bind:value={vocabMode} class={SELECT_CLASS}>
							<option value="substring">substring</option>
							<option value="prefix">prefix</option>
							<option value="exact">exact</option>
						</select>
						<Button size="sm" type="submit">Search</Button>
					</form>
					{#if vocabError}<p class="text-xs text-destructive">{vocabError}</p>{/if}
					{#if vocabResults.length}
						<p class="text-[10px] text-muted-foreground">Click to add to {rs.draft.groups[activeGroupIdx]?.name ?? 'no group'}.</p>
					{/if}
					<div class="flex max-h-40 flex-wrap gap-1 overflow-y-auto">
						{#each vocabResults as t (t.id)}
							<button
								class="rounded border px-1 py-0.5 text-xs hover:bg-muted"
								title="type {t.type}"
								disabled={!rs.draft.groups[activeGroupIdx]}
								onclick={() => {
									const g = rs.draft.groups[activeGroupIdx];
									if (g) addTokenId(g, t.id);
								}}
							>
								<ResearchPiece piece={t.piece} b64={t.piece_b64} /> <span class="text-muted-foreground">{t.id}</span>
							</button>
						{/each}
					</div>
					<form
						class="flex gap-2"
						onsubmit={(e) => {
							e.preventDefault();
							void tokenize();
						}}
					>
						<Input bind:value={tokText} placeholder="tokenize text, e.g. The capital of France is Paris" class="h-8" />
						<Button size="sm" type="submit">Tokenize</Button>
					</form>
					<div class="flex flex-wrap gap-1">
						{#each tokResults as t, i (i)}
							<span class="rounded border px-1 py-0.5 text-xs">
								<ResearchPiece piece={t.piece} b64={t.piece_b64} /> <span class="text-muted-foreground">{t.id}</span>
							</span>
						{/each}
					</div>
				</div>
			</details>
		</div>

		<div class="min-w-0">
			{#if rs.preview}
				<ResearchCompilePreview plan={rs.preview} groups={rs.draft.groups} onUseFirst={(gid, tid) => rs.useFirstToken(gid, tid)} />
			{:else}
				<div class="rounded-lg border border-dashed p-6 text-center text-sm text-muted-foreground">
					Compile preview resolves each piece to token ids with this model's tokenizer.
				</div>
			{/if}
		</div>
	</div>
</div>
