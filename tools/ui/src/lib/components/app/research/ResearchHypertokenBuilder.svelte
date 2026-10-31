<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { ResearchService } from '$lib/services/research.service';
	import { errMsg, researchStore as rs } from '$lib/stores/research.svelte';
	import type { DesignGroup, TokenInfo } from '$lib/types/research';
	import { visiblePiece } from '$lib/utils/research';
	import ResearchCompilePreview from './ResearchCompilePreview.svelte';
	import ResearchPiece from './ResearchPiece.svelte';

	const SELECT = 'h-8 rounded-md border border-input bg-transparent px-2 text-sm dark:bg-input/30';

	let newGroupName = $state('');
	let activeGroupIdx = $state(0);

	let vocabQuery = $state('');
	let vocabMode = $state<'substring' | 'exact' | 'prefix'>('substring');
	let vocabResults = $state.raw<TokenInfo[]>([]);
	let vocabError = $state<string | null>(null);

	let tokText = $state('The capital of France is Paris');
	let tokResults = $state.raw<TokenInfo[]>([]);

	let design = $derived(rs.design);
	let immutable = $derived(design?.state === 'approved');

	function addGroup() {
		const name = newGroupName.trim() || `GROUP_${rs.draft.groups.length + 1}`;
		rs.addGroup(name, rs.draft.groups.length === 0 ? 'target' : 'competitor');
		activeGroupIdx = rs.draft.groups.length - 1;
		newGroupName = '';
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

	async function saveRevision() {
		await rs.saveDesign();
	}
</script>

<div class="grid gap-6 2xl:grid-cols-[minmax(0,3fr)_minmax(0,2fr)]">
	<div class="space-y-4">
		<div class="flex flex-wrap items-center gap-2">
			<h2 class="text-lg font-semibold">Hypertoken groups</h2>
			<span class="text-xs text-muted-foreground">
				A group is a set of single next-token IDs; P(H|c) = sum of member probabilities.
			</span>
		</div>

		<div class="flex flex-wrap items-center gap-2 text-sm">
			<span class="text-muted-foreground">Revisions:</span>
			{#each rs.designs as d (d.id)}
				<Button size="sm" variant={rs.designId === d.id ? 'secondary' : 'ghost'} onclick={() => rs.loadDesignIntoDraft(d)}>
					r{d.revision ?? '?'} <Badge variant={d.state === 'approved' ? 'default' : 'outline'}>{d.state}</Badge>
				</Button>
			{:else}
				<span class="text-xs text-muted-foreground">none saved yet</span>
			{/each}
		</div>
		{#if immutable}
			<p class="text-xs text-amber-600">
				Loaded revision is approved and immutable. Edits here are saved as a new draft revision.
			</p>
		{/if}

		{#each rs.draft.groups as g, gi (g.id)}
			<div
				class={['space-y-2 rounded-lg border p-3', activeGroupIdx === gi && 'ring-2 ring-ring/40']}
				style:border-left={`4px solid ${g.color ?? '#888'}`}
			>
				<div class="flex flex-wrap items-center gap-2">
					<input type="color" bind:value={g.color} class="h-8 w-8 cursor-pointer rounded border" title="Group color (UI only)" />
					<Input bind:value={g.name} class="h-8 w-40" />
					<select bind:value={g.role} class={SELECT}>
						<option value="target">target</option>
						<option value="competitor">competitor</option>
						<option value="control">control</option>
					</select>
					<code class="text-xs text-muted-foreground">id {g.id}</code>
					<div class="ml-auto flex gap-1">
						<Button size="sm" variant={activeGroupIdx === gi ? 'secondary' : 'ghost'} onclick={() => (activeGroupIdx = gi)}
							>{activeGroupIdx === gi ? 'Active for vocab clicks' : 'Make active'}</Button
						>
						<Button size="sm" variant="ghost" onclick={() => removeGroup(gi)}>Remove</Button>
					</div>
				</div>

				{#each g.selectors as s, si (si)}
					<div class="flex flex-wrap items-start gap-2 text-sm">
						<Badge variant="tertiary" class="mt-1">{s.kind}</Badge>
						{#if s.kind === 'exact_token_piece'}
							<div class="flex-1 space-y-1">
								<Input bind:value={s.text} class="h-8 font-mono" />
								<div class="text-xs text-muted-foreground">
									shown as <ResearchPiece piece={s.text} /> (spaces marked)
								</div>
							</div>
						{:else if s.kind === 'variant_list'}
							<div class="flex-1 space-y-1">
								<textarea
									class="min-h-16 w-full rounded-md border border-input bg-transparent px-2 py-1 font-mono text-sm dark:bg-input/30"
									value={s.texts.join('\n')}
									oninput={(e) => (s.texts = e.currentTarget.value.split('\n'))}
									placeholder="one variant per line; leading spaces are significant"
								></textarea>
								<div class="flex flex-wrap gap-1">
									{#each s.texts.filter((t) => t.length > 0) as t, ti (ti)}<ResearchPiece piece={t} />{/each}
								</div>
							</div>
						{:else}
							<div class="flex-1 space-y-1">
								<Input
									class="h-8 font-mono"
									value={s.ids.join(', ')}
									onchange={(e) => (s.ids = parseIds(e.currentTarget.value))}
									placeholder="token ids, comma separated"
								/>
							</div>
						{/if}
						<Button size="sm" variant="ghost" onclick={() => g.selectors.splice(si, 1)}>x</Button>
					</div>
				{/each}

				<div class="flex flex-wrap gap-1">
					<Button size="sm" variant="outline" onclick={() => g.selectors.push({ kind: 'exact_token_piece', text: ' ' })}
						>+ exact piece</Button
					>
					<Button size="sm" variant="outline" onclick={() => g.selectors.push({ kind: 'variant_list', texts: [] })}
						>+ variant list</Button
					>
					<Button size="sm" variant="outline" onclick={() => g.selectors.push({ kind: 'token_ids', ids: [] })}
						>+ token ids</Button
					>
				</div>
			</div>
		{/each}

		<div class="flex flex-wrap items-center gap-2">
			<Input bind:value={newGroupName} placeholder="New group name, e.g. PARIS" class="h-8 w-56" />
			<Button size="sm" variant="secondary" onclick={addGroup}>Add group</Button>
		</div>

		<label class="flex items-center gap-2 text-sm">
			<span class="text-muted-foreground">Extra watched token IDs</span>
			<Input
				class="h-8 w-64 font-mono"
				value={rs.draft.watch_extra_token_ids.join(', ')}
				onchange={(e) => (rs.draft.watch_extra_token_ids = parseIds(e.currentTarget.value))}
			/>
		</label>

		<div class="flex flex-wrap gap-2 border-t pt-3">
			<Button size="sm" variant="outline" onclick={() => rs.compilePreview()} disabled={rs.draft.groups.length === 0}
				>Compile preview</Button
			>
			<Button size="sm" onclick={saveRevision} disabled={rs.draft.groups.length === 0}>Save as new design revision</Button>
			<Button size="sm" variant="outline" onclick={() => rs.compileOnly()} disabled={!rs.designId}>Compile saved</Button>
			<Button size="sm" variant="secondary" onclick={() => rs.compileAndApprove()} disabled={!rs.designId || immutable}
				>Compile + approve</Button
			>
			{#if design}
				<span class="self-center text-xs text-muted-foreground">selected {design.id} ({design.state})</span>
			{/if}
		</div>

		{#if rs.preview}
			<ResearchCompilePreview plan={rs.preview} groups={rs.draft.groups} />
		{/if}
	</div>

	<aside class="space-y-4">
		<section class="space-y-2 rounded-lg border p-3">
			<h3 class="font-medium">Vocabulary search</h3>
			<p class="text-xs text-muted-foreground">
				Click a token to add its ID to the active group
				({rs.draft.groups[activeGroupIdx]?.name ?? 'no group'}).
			</p>
			<form
				class="flex gap-2"
				onsubmit={(e) => {
					e.preventDefault();
					void searchVocab();
				}}
			>
				<Input bind:value={vocabQuery} placeholder="e.g. Paris" class="h-8" />
				<select bind:value={vocabMode} class={SELECT}>
					<option value="substring">substring</option>
					<option value="prefix">prefix</option>
					<option value="exact">exact</option>
				</select>
				<Button size="sm" type="submit">Search</Button>
			</form>
			{#if vocabError}<p class="text-xs text-destructive">{vocabError}</p>{/if}
			<div class="flex max-h-72 flex-wrap gap-1 overflow-y-auto">
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
		</section>

		<section class="space-y-2 rounded-lg border p-3">
			<h3 class="font-medium">Tokenize preview</h3>
			<form
				class="flex gap-2"
				onsubmit={(e) => {
					e.preventDefault();
					void tokenize();
				}}
			>
				<Input bind:value={tokText} class="h-8" />
				<Button size="sm" type="submit">Tokenize</Button>
			</form>
			<div class="flex flex-wrap gap-1">
				{#each tokResults as t, i (i)}
					<span class="rounded border px-1 py-0.5 text-xs" title={visiblePiece(t.piece)}>
						<ResearchPiece piece={t.piece} b64={t.piece_b64} /> <span class="text-muted-foreground">{t.id}</span>
					</span>
				{/each}
			</div>
			{#if tokResults.length}
				<p class="text-xs text-muted-foreground">
					Words that split into several tokens cannot be a single hypertoken member (multi_token).
				</p>
			{/if}
		</section>
	</aside>
</div>
