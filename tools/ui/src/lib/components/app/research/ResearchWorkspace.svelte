<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { Textarea } from '$lib/components/ui/textarea';
	import { researchStore as rs, type ResearchTab } from '$lib/stores/research.svelte';
	import { baseName } from '$lib/utils/research';
	import { onMount } from 'svelte';
	import ResearchHypertokenBuilder from './ResearchHypertokenBuilder.svelte';
	import ResearchInterventionBuilder from './ResearchInterventionBuilder.svelte';
	import ResearchQuestionPanel from './ResearchQuestionPanel.svelte';
	import ResearchRunCompare from './ResearchRunCompare.svelte';
	import ResearchRunLauncher from './ResearchRunLauncher.svelte';
	import ResearchRunLibrary from './ResearchRunLibrary.svelte';

	const TABS: { id: ResearchTab; label: string }[] = [
		{ id: 'question', label: '1. Question & AI design' },
		{ id: 'design', label: '2. Hypertokens' },
		{ id: 'interventions', label: '3. Interventions' },
		{ id: 'launch', label: '4. Launch runs' },
		{ id: 'runs', label: '5. Runs & inspector' },
		{ id: 'compare', label: '6. Compare' }
	];

	let newProjectTitle = $state('');
	let newProjectDesc = $state('');
	let newQuestionText = $state('');
	let newHypothesis = $state('');
	let showNewProject = $state(false);
	let showNewQuestion = $state(false);

	onMount(() => {
		void rs.init();
		return () => {
			for (const v of rs.views.values()) v.stop();
		};
	});

	function openTab(id: ResearchTab) {
		if (id === 'compare') rs.prefillCompare();
		rs.tab = id;
	}

	async function createProject() {
		if (!newProjectTitle.trim()) return;
		await rs.createProject(newProjectTitle.trim(), newProjectDesc.trim());
		newProjectTitle = '';
		newProjectDesc = '';
		showNewProject = false;
	}

	async function createQuestion() {
		if (!newQuestionText.trim()) return;
		await rs.createQuestion({
			question_text: newQuestionText.trim(),
			hypothesis: newHypothesis.trim() || undefined
		});
		newQuestionText = '';
		newHypothesis = '';
		showNewQuestion = false;
	}
</script>

<div class="flex h-[100dvh] flex-col">
	<header class="flex flex-wrap items-center gap-3 border-b px-4 pt-12 pb-3 md:px-6 md:pt-4">
		<h1 class="text-xl font-semibold tracking-tight">Research</h1>
		<span class="text-xs text-muted-foreground">Next-token hypertokens and logit interventions (POC)</span>

		<div class="ml-auto flex min-w-0 flex-wrap items-center gap-2 text-xs">
			{#if rs.status === null}
				<Badge variant="outline">checking status...</Badge>
			{:else if rs.status.enabled}
				<Badge variant="secondary">enabled</Badge>
				<span class="max-w-[22rem] truncate font-medium" title={rs.status.model_id}>{baseName(rs.status.model_id)}</span>
				<span
					class="cursor-help text-muted-foreground underline decoration-dotted"
					title="tokenizer {rs.status.tokenizer_fingerprint ?? '-'}">n_vocab {rs.status.n_vocab}</span
				>
			{:else}
				<Badge variant="destructive">disabled</Badge>
			{/if}
		</div>
	</header>

	{#if rs.status && !rs.status.enabled}
		<div class="mx-auto mt-10 max-w-2xl space-y-3 rounded-lg border p-6 text-sm">
			<h2 class="text-lg font-semibold">Research module is not enabled on this server</h2>
			<p>
				The research API (<code>/api/research/v1</code>) is only available when llama-server is
				built with <code>-DLLAMA_SERVER_RESEARCH=ON</code> and started with
				<code>--research-db PATH</code>:
			</p>
			<pre class="overflow-x-auto rounded bg-muted p-3 text-xs">llama-server -m model.gguf --research-db research.sqlite --port 8080</pre>
			{#if rs.statusError}
				<p class="text-xs text-muted-foreground">Server said: {rs.statusError}</p>
			{/if}
			<Button size="sm" variant="outline" onclick={() => rs.init()}>Retry</Button>
		</div>
	{:else if rs.status}
		<div class="flex min-h-0 flex-1 flex-col md:flex-row">
			<aside class="w-full shrink-0 space-y-4 overflow-y-auto border-b p-4 md:w-64 md:border-r md:border-b-0">
				<section class="space-y-2">
					<div class="flex items-center justify-between">
						<h2 class="text-xs font-semibold tracking-wide text-muted-foreground uppercase">Projects</h2>
						<Button size="sm" variant="ghost" class="h-6 px-2 text-xs" onclick={() => (showNewProject = !showNewProject)}
							>{showNewProject ? 'Cancel' : '+ New'}</Button
						>
					</div>
					{#if showNewProject}
						<div class="space-y-2 rounded border p-2">
							<Input bind:value={newProjectTitle} placeholder="Project title" class="h-8" />
							<Input bind:value={newProjectDesc} placeholder="Description (optional)" class="h-8" />
							<Button size="sm" variant="secondary" class="w-full" onclick={createProject} disabled={!newProjectTitle.trim()}
								>Create project</Button
							>
						</div>
					{/if}
					<ul class="space-y-1">
						{#each rs.projects as p (p.id)}
							<li>
								<button
									class={[
										'w-full truncate rounded px-2 py-1 text-left text-sm hover:bg-muted',
										rs.project?.id === p.id && 'bg-accent font-medium'
									]}
									title={p.title}
									onclick={() => rs.selectProject(p.id)}>{p.title}</button
								>
							</li>
						{:else}
							<li class="text-xs text-muted-foreground">No projects yet.</li>
						{/each}
					</ul>
				</section>

				{#if rs.project}
					<section class="space-y-2">
						<div class="flex items-center justify-between gap-2">
							<h2 class="text-xs font-semibold tracking-wide text-muted-foreground uppercase">Questions</h2>
							<Button size="sm" variant="ghost" class="h-6 px-2 text-xs" onclick={() => (showNewQuestion = !showNewQuestion)}
								>{showNewQuestion ? 'Cancel' : '+ New'}</Button
							>
						</div>
						{#if showNewQuestion}
							<div class="space-y-2 rounded border p-2">
								<Textarea bind:value={newQuestionText} placeholder="Research question" class="min-h-16 text-sm" />
								<Textarea bind:value={newHypothesis} placeholder="Hypothesis (optional)" class="min-h-12 text-sm" />
								<Button size="sm" variant="secondary" class="w-full" onclick={createQuestion} disabled={!newQuestionText.trim()}
									>Create question</Button
								>
							</div>
						{/if}
						<ul class="space-y-1">
							{#each rs.project.questions ?? [] as q (q.id)}
								<li>
									<button
										class={[
											'line-clamp-2 w-full rounded px-2 py-1 text-left text-sm hover:bg-muted',
											rs.question?.id === q.id && 'bg-accent font-medium'
										]}
										title={q.question_text}
										onclick={() => rs.selectQuestion(q.id)}>{q.question_text}</button
									>
								</li>
							{:else}
								<li class="text-xs text-muted-foreground">No questions yet.</li>
							{/each}
						</ul>
					</section>
				{/if}
			</aside>

			<main class="flex min-h-0 min-w-0 flex-1 flex-col">
				{#if rs.error}
					<div class="m-3 flex items-start gap-2 rounded border border-destructive/50 bg-destructive/10 p-2 text-sm">
						<span class="flex-1 break-words">{rs.error}</span>
						<Button size="sm" variant="ghost" onclick={() => (rs.error = null)}>Dismiss</Button>
					</div>
				{/if}

				{#if !rs.question}
					<div class="p-6 text-sm text-muted-foreground">
						Select or create a project and a research question to start.
					</div>
				{:else}
					<nav class="flex flex-wrap gap-1 border-b px-3 py-2">
						{#each TABS as t (t.id)}
							<Button size="sm" variant={rs.tab === t.id ? 'secondary' : 'ghost'} onclick={() => openTab(t.id)}
								>{t.label}</Button
							>
						{/each}
						{#if rs.busy}
							<span class="ml-auto self-center text-xs text-muted-foreground">working...</span>
						{/if}
					</nav>

					<div class="min-h-0 flex-1 overflow-y-auto p-4">
						{#if rs.tab === 'question'}
							{#key rs.question.id}<ResearchQuestionPanel />{/key}
						{:else if rs.tab === 'design'}
							<ResearchHypertokenBuilder />
						{:else if rs.tab === 'interventions'}
							<ResearchInterventionBuilder />
						{:else if rs.tab === 'launch'}
							<ResearchRunLauncher />
						{:else if rs.tab === 'runs'}
							<ResearchRunLibrary />
						{:else}
							<ResearchRunCompare />
						{/if}
					</div>
				{/if}
			</main>
		</div>
	{/if}
</div>
