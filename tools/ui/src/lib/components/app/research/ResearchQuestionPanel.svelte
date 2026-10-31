<script lang="ts" module>
	import type { DesignProposal } from '$lib/types/research';

	// System instruction from design doc 08 (short version), tightened for single pieces.
	const SYSTEM_PROMPT =
		"Propose next-token measurement groups and logit interventions for the user's research question. " +
		'Return valid structured JSON. Never fabricate token IDs. ' +
		'candidate_pieces must be SINGLE words or single word-pieces as a tokenizer emits them for the next token: ' +
		'never phrases, never sentences, no trailing spaces, at most 4 per group. ' +
		'Always include the leading-space variant (e.g. " Paris" next to "Paris") and the main capitalization variant. ' +
		'Make exactly one group per concept (answer category). The group name is that concept in UPPERCASE, e.g. NEGATIVE; ' +
		'the role (target, competitor or control) goes in the separate role field, never in the name. ' +
		'A piece may appear in only one group. ' +
		'Interventions only use operation add_logit_bias on target groups; delta is a number (negative suppresses). Never propose exclude. ' +
		'List at most 3 limitations, each one short sentence: why next-token mass may not capture the full answer or semantic belief.';

	// Structured output shape of doc 08, as a JSON schema for response_format.
	const PROPOSAL_SCHEMA = {
		type: 'object',
		additionalProperties: false,
		required: [
			'schema_version',
			'research_question',
			'hypotheses',
			'observation_groups',
			'intervention_proposals',
			'controls',
			'measurement_plan',
			'limitations'
		],
		properties: {
			schema_version: { type: 'integer' },
			research_question: { type: 'string' },
			hypotheses: {
				type: 'array',
				maxItems: 3,
				items: {
					type: 'object',
					additionalProperties: false,
					required: ['id', 'text'],
					properties: { id: { type: 'string' }, text: { type: 'string' } }
				}
			},
			observation_groups: {
				type: 'array',
				minItems: 1,
				maxItems: 5,
				items: {
					type: 'object',
					additionalProperties: false,
					required: ['name', 'role', 'candidate_pieces'],
					properties: {
						name: { type: 'string', pattern: '^[A-Z][A-Z0-9_]{0,31}$' },
						role: { type: 'string', enum: ['target', 'competitor', 'control'] },
						candidate_pieces: {
							type: 'array',
							minItems: 1,
							maxItems: 4,
							// one word or word-piece, optional single leading space
							items: { type: 'string', pattern: '^ ?[^ \\t\\n]{1,24}$' }
						}
					}
				}
			},
			intervention_proposals: {
				type: 'array',
				maxItems: 3,
				items: {
					type: 'object',
					additionalProperties: false,
					required: ['target_group', 'operation', 'delta'],
					properties: {
						target_group: { type: 'string' },
						operation: { type: 'string', enum: ['add_logit_bias'] },
						delta: { type: 'number' }
					}
				}
			},
			controls: {
				type: 'object',
				additionalProperties: false,
				required: ['include_unmodified', 'matched_prompts', 'repeat_seeds'],
				properties: {
					include_unmodified: { type: 'boolean' },
					matched_prompts: { type: 'boolean' },
					repeat_seeds: { type: 'integer' }
				}
			},
			measurement_plan: {
				type: 'object',
				additionalProperties: false,
				required: ['capture_every_n', 'required_stages'],
				properties: {
					capture_every_n: { type: 'integer' },
					required_stages: { type: 'array', items: { type: 'string' } }
				}
			},
			limitations: { type: 'array', maxItems: 3, items: { type: 'string', maxLength: 160 } }
		}
	};

	/** Validate untrusted LLM output against the suggestion schema. Returns errors. */
	function validateProposal(v: unknown): string[] {
		const errs: string[] = [];
		const o = v as Partial<DesignProposal>;
		if (!o || typeof o !== 'object') return ['not a JSON object'];
		if (!Array.isArray(o.hypotheses)) errs.push('hypotheses must be an array');
		if (!Array.isArray(o.observation_groups) || o.observation_groups.length === 0)
			errs.push('observation_groups must be a non-empty array');
		else
			o.observation_groups.forEach((g, i) => {
				if (typeof g?.name !== 'string' || !g.name) errs.push(`observation_groups[${i}].name missing`);
				if (!['target', 'competitor', 'control'].includes(g?.role as string))
					errs.push(`observation_groups[${i}].role invalid`);
				if (!Array.isArray(g?.candidate_pieces) || g.candidate_pieces.some((p) => typeof p !== 'string'))
					errs.push(`observation_groups[${i}].candidate_pieces must be strings`);
			});
		if (!Array.isArray(o.intervention_proposals)) errs.push('intervention_proposals must be an array');
		else
			o.intervention_proposals.forEach((r, i) => {
				if (typeof r?.delta !== 'number') errs.push(`intervention_proposals[${i}].delta must be a number`);
			});
		if (!Array.isArray(o.limitations)) errs.push('limitations must be an array');
		return errs;
	}
</script>

<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Textarea } from '$lib/components/ui/textarea';
	import { chatCompletionJson, ResearchService } from '$lib/services/research.service';
	import { cleanPieces, errMsg, researchStore as rs } from '$lib/stores/research.svelte';
	import type { ResearchProposal } from '$lib/types/research';
	import { shortId, signed } from '$lib/utils/research';
	import ResearchBatchMatrix from './ResearchBatchMatrix.svelte';
	import ResearchPiece from './ResearchPiece.svelte';

	// local edit buffer; the workspace remounts this panel per question ({#key})
	let questionText = $state(rs.question?.question_text ?? '');
	let hypothesis = $state(rs.question?.hypothesis ?? '');
	let notes = $state(rs.question?.notes ?? '');

	let suggesting = $state(false);
	let suggestError = $state<string | null>(null);
	let rawOutput = $state('');
	let useSchema = $state(true);
	let current = $state.raw<ResearchProposal | null>(rs.proposals[rs.proposals.length - 1] ?? null);

	// show what Apply will use: cleaned pieces, bias rules only
	let shown = $derived(current ? sanitize(current.proposal) : null);
	let targetNames = $derived(
		new Set(shown?.observation_groups.filter((g) => g.role === 'target').map((g) => g.name) ?? [])
	);

	// pieces proposed for more than one group (ambiguous evidence)
	let sharedPieces = $derived.by(() => {
		const seen = new Map<string, string[]>();
		for (const g of shown?.observation_groups ?? [])
			for (const piece of g.candidate_pieces) seen.set(piece, [...(seen.get(piece) ?? []), g.name]);
		return [...seen].filter(([, names]) => names.length > 1);
	});

	const clip = (t: string, n = 140) => (t.length > n ? `${t.slice(0, n - 3).trimEnd()}...` : t);

	function openRun(id: string) {
		rs.openRun(id);
		rs.selectedRunId = id;
		rs.tab = 'runs';
	}

	/** Normalize untrusted output: clean pieces, keep only bias rules. */
	function sanitize(p: DesignProposal): DesignProposal {
		return {
			...p,
			observation_groups: p.observation_groups.map((g) => ({ ...g, candidate_pieces: cleanPieces(g.candidate_pieces) })),
			intervention_proposals: p.intervention_proposals.filter((r) => r.operation === 'add_logit_bias')
		};
	}

	async function suggest() {
		if (!rs.question) return;
		suggesting = true;
		suggestError = null;
		rawOutput = '';
		const userPrompt =
			`Research question: ${questionText}\n` +
			(hypothesis ? `Hypothesis: ${hypothesis}\n` : '') +
			(notes ? `Notes: ${notes}\n` : '') +
			`Target model: ${rs.status?.model_id ?? 'unknown'} (n_vocab ${rs.status?.n_vocab ?? '?'}).\n` +
			'Constraints: at most 5 groups, at most 4 single-word candidate pieces per group, one-word answers only.';
		try {
			const res = await chatCompletionJson(
				[
					{ role: 'system', content: SYSTEM_PROMPT },
					{ role: 'user', content: userPrompt }
				],
				useSchema ? PROPOSAL_SCHEMA : null
			);
			rawOutput = res.content;
			let parsed: unknown;
			try {
				parsed = JSON.parse(res.content);
			} catch {
				throw new Error('model output is not valid JSON (raw output shown below)');
			}
			const errs = validateProposal(parsed);
			if (errs.length) throw new Error(`proposal failed validation: ${errs.join('; ')}`);
			current = await ResearchService.createProposal({
				question_id: rs.question.id,
				model: res.model,
				prompt: `${SYSTEM_PROMPT}\n---\n${userPrompt}`,
				raw_output: res.content,
				proposal: sanitize(parsed as DesignProposal)
			});
			rs.proposals = [...rs.proposals, current];
		} catch (e) {
			suggestError = errMsg(e);
		} finally {
			suggesting = false;
		}
	}

	function apply() {
		if (!current) return;
		rs.applyProposal(sanitize(structuredClone(current.proposal)));
	}
</script>

<div class="space-y-4">
	<section class="space-y-2">
		<div class="grid gap-3 lg:grid-cols-3">
			<label class="block space-y-1 text-sm">
				<span class="text-muted-foreground">Question</span>
				<Textarea bind:value={questionText} class="min-h-24" />
			</label>
			<label class="block space-y-1 text-sm">
				<span class="text-muted-foreground">Hypothesis</span>
				<Textarea bind:value={hypothesis} class="min-h-24" />
			</label>
			<label class="block space-y-1 text-sm">
				<span class="text-muted-foreground">Notes</span>
				<Textarea bind:value={notes} class="min-h-24" />
			</label>
		</div>
		<div class="flex flex-wrap items-center gap-2">
			<Button size="sm" variant="outline" onclick={() => rs.saveQuestion({ question_text: questionText, hypothesis, notes })}
				>Save question</Button
			>
			<Button size="sm" onclick={suggest} disabled={suggesting}>{suggesting ? 'Asking model...' : 'Suggest design with AI'}</Button>
			<span class="text-xs text-muted-foreground">The model proposes text pieces only; the tokenizer resolves IDs at compile.</span>
			<details class="ml-auto text-xs">
				<summary class="text-muted-foreground">advanced</summary>
				<label class="mt-1 flex items-center gap-1">
					<input type="checkbox" bind:checked={useSchema} /> json_schema (off: json_object)
				</label>
			</details>
		</div>
		{#if rs.proposals.length}
			<div class="flex flex-wrap items-center gap-1 text-xs">
				<span class="text-muted-foreground">Proposals:</span>
				{#each rs.proposals as p (p.id)}
					<Button
						size="sm"
						variant={current?.id === p.id ? 'secondary' : 'ghost'}
						class="h-6 px-2 text-xs"
						onclick={() => (current = p)}>{shortId(p.id)}</Button
					>
				{/each}
			</div>
		{/if}
		{#if suggestError}
			<div class="rounded border border-destructive/50 bg-destructive/10 p-2 text-sm">{suggestError}</div>
		{/if}
		{#if rawOutput && suggestError}
			<pre class="max-h-48 overflow-auto rounded bg-muted p-2 text-xs whitespace-pre-wrap">{rawOutput}</pre>
		{/if}
	</section>

	{#if rs.runs.length}
		<div class="rounded-lg border p-3">
			<ResearchBatchMatrix compact onPick={openRun} />
		</div>
	{/if}

	{#if current && shown}
		{@const p = shown}
		<section class="min-w-0 space-y-3 rounded-lg border p-3 text-sm">
			<div class="flex flex-wrap items-center gap-2">
				<h3 class="font-medium">AI proposal</h3>
				<Badge variant="outline">untrusted draft</Badge>
				<span class="truncate text-xs text-muted-foreground" title={current.model}>{shortId(current.id)}</span>
				<div class="ml-auto flex gap-2">
					<Button size="sm" onclick={apply}>Apply draft</Button>
					<Button size="sm" variant="ghost" onclick={() => (current = null)}>Reject</Button>
				</div>
			</div>

			<div class="grid gap-2 md:grid-cols-2 xl:grid-cols-3">
				{#each p.observation_groups as g, i (i)}
					<div class="min-w-0 rounded border px-2 py-1.5">
						<div class="flex items-center gap-2">
							<span class="truncate font-medium" title={g.name}>{g.name}</span>
							<Badge variant="outline" class="text-[10px]">{g.role}</Badge>
						</div>
						<div class="mt-1 flex flex-wrap gap-1">
							{#each g.candidate_pieces as piece, j (j)}<ResearchPiece {piece} />{/each}
						</div>
					</div>
				{/each}
			</div>

			{#if sharedPieces.length}
				<p class="text-xs text-amber-600">
					Pieces shared across groups:
					{#each sharedPieces as [piece, names] (piece)}<span class="ml-2"><ResearchPiece {piece} /> {names.join(' / ')}</span>{/each}
				</p>
			{/if}

			<div class="grid gap-3 text-xs md:grid-cols-3">
				<div class="min-w-0">
					<h4 class="font-medium text-muted-foreground">Hypotheses</h4>
					<ul class="list-disc pl-4">
						{#each p.hypotheses as h, i (i)}<li>{h.text}</li>{/each}
					</ul>
				</div>
				<div class="min-w-0">
					<h4 class="font-medium text-muted-foreground">Interventions (applied: targets only)</h4>
					<ul class="list-disc pl-4">
						{#each p.intervention_proposals as r, i (i)}
							<li class={[!targetNames.has(r.target_group) && 'text-muted-foreground line-through']}>
								{r.target_group} {signed(r.delta)}
							</li>
						{:else}
							<li class="text-muted-foreground">none</li>
						{/each}
					</ul>
				</div>
				<div class="min-w-0">
					<h4 class="font-medium text-muted-foreground">Limitations</h4>
					<ul class="list-disc pl-4">
						{#each p.limitations.slice(0, 3) as l, i (i)}<li title={l}>{clip(l)}</li>{/each}
					</ul>
				</div>
			</div>
		</section>
	{/if}
</div>
