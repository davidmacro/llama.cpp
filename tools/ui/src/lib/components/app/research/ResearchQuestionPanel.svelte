<script lang="ts" module>
	import type { DesignProposal } from '$lib/types/research';

	// System instruction from design doc 08 (short version).
	const SYSTEM_PROMPT =
		"Propose next-token measurement groups and controlled logit interventions from the user's research question. " +
		'Return valid structured JSON. Never fabricate actual token IDs. Mark multi-token uncertainty. ' +
		'Propose competitors and controls. Describe why the measurement may not capture semantic beliefs or full answer sequences. ' +
		'candidate_pieces are literal text pieces as a tokenizer would emit them for the next token; include leading-space variants (e.g. " Paris") and capitalization variants. ' +
		'role is one of target, competitor, control. operation is add_logit_bias or exclude; delta is a number (negative suppresses).';

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
				items: {
					type: 'object',
					additionalProperties: false,
					required: ['id', 'text'],
					properties: { id: { type: 'string' }, text: { type: 'string' } }
				}
			},
			observation_groups: {
				type: 'array',
				items: {
					type: 'object',
					additionalProperties: false,
					required: ['name', 'role', 'candidate_pieces'],
					properties: {
						name: { type: 'string' },
						role: { type: 'string', enum: ['target', 'competitor', 'control'] },
						candidate_pieces: { type: 'array', items: { type: 'string' } }
					}
				}
			},
			intervention_proposals: {
				type: 'array',
				items: {
					type: 'object',
					additionalProperties: false,
					required: ['target_group', 'operation', 'delta'],
					properties: {
						target_group: { type: 'string' },
						operation: { type: 'string', enum: ['add_logit_bias', 'exclude'] },
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
			limitations: { type: 'array', items: { type: 'string' } }
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
				if (!['add_logit_bias', 'exclude'].includes(r?.operation as string))
					errs.push(`intervention_proposals[${i}].operation invalid`);
				if (typeof r?.delta !== 'number') errs.push(`intervention_proposals[${i}].delta must be a number`);
			});
		if (!Array.isArray(o.limitations)) errs.push('limitations must be an array');
		return errs;
	}
</script>

<script lang="ts">
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { Textarea } from '$lib/components/ui/textarea';
	import { chatCompletionJson, ResearchService } from '$lib/services/research.service';
	import { errMsg, researchStore as rs } from '$lib/stores/research.svelte';
	import type { ResearchProposal } from '$lib/types/research';
	import ResearchPiece from './ResearchPiece.svelte';

	// local edit buffer; the workspace remounts this panel per question ({#key})
	let questionText = $state(rs.question?.question_text ?? '');
	let hypothesis = $state(rs.question?.hypothesis ?? '');
	let notes = $state(rs.question?.notes ?? '');

	let suggesting = $state(false);
	let suggestError = $state<string | null>(null);
	let rawOutput = $state('');
	let useSchema = $state(true);
	let current = $state.raw<ResearchProposal | null>(null);

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
			'Constraints: at most 5 groups, at most 8 candidate pieces per group, breadth balanced.';
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
				proposal: parsed as DesignProposal
			});
			rs.proposals = [...rs.proposals, current];
		} catch (e) {
			suggestError = errMsg(e);
		} finally {
			suggesting = false;
		}
	}
</script>

<div class="grid gap-6 xl:grid-cols-2">
	<section class="space-y-3">
		<h2 class="text-lg font-semibold">Research question</h2>
		<label class="block space-y-1 text-sm">
			<span class="text-muted-foreground">Question</span>
			<Textarea bind:value={questionText} class="min-h-16" />
		</label>
		<label class="block space-y-1 text-sm">
			<span class="text-muted-foreground">Hypothesis</span>
			<Input bind:value={hypothesis} />
		</label>
		<label class="block space-y-1 text-sm">
			<span class="text-muted-foreground">Notes</span>
			<Textarea bind:value={notes} class="min-h-16" />
		</label>
		<div class="flex items-center gap-2">
			<Button size="sm" onclick={() => rs.saveQuestion({ question_text: questionText, hypothesis, notes })}>Save question</Button>
			<span class="text-xs text-muted-foreground">id {rs.question?.id} · status {rs.question?.status ?? '-'}</span>
		</div>
	</section>

	<section class="space-y-3">
		<h2 class="text-lg font-semibold">AI design assistant</h2>
		<p class="text-xs text-muted-foreground">
			Sends the question to this server's own <code>/v1/chat/completions</code> (temperature 0.3,
			structured JSON). The LLM proposes groups and text pieces only; token IDs are always resolved
			by the server's tokenizer in the compile step. Proposals are stored as untrusted drafts.
		</p>
		<div class="flex flex-wrap items-center gap-3">
			<Button size="sm" onclick={suggest} disabled={suggesting}>{suggesting ? 'Asking model...' : 'Suggest design'}</Button>
			<label class="flex items-center gap-1 text-xs">
				<input type="checkbox" bind:checked={useSchema} /> json_schema (off: json_object)
			</label>
		</div>
		{#if suggestError}
			<div class="rounded border border-destructive/50 bg-destructive/10 p-2 text-sm">{suggestError}</div>
		{/if}
		{#if rawOutput && suggestError}
			<pre class="max-h-48 overflow-auto rounded bg-muted p-2 text-xs whitespace-pre-wrap">{rawOutput}</pre>
		{/if}

		{#if rs.proposals.length}
			<div class="flex flex-wrap items-center gap-1 text-xs">
				<span class="text-muted-foreground">Stored proposals:</span>
				{#each rs.proposals as p (p.id)}
					<Button size="sm" variant={current?.id === p.id ? 'secondary' : 'ghost'} onclick={() => (current = p)}
						>{p.id}</Button
					>
				{/each}
			</div>
		{/if}

		{#if current}
			{@const p = current.proposal}
			<div class="space-y-3 rounded-lg border p-3 text-sm">
				<div class="flex items-center gap-2">
					<Badge variant="outline">untrusted draft</Badge>
					<span class="text-xs text-muted-foreground">model {current.model ?? '-'}</span>
					<div class="ml-auto flex gap-2">
						<Button size="sm" onclick={() => rs.applyProposal($state.snapshot(p) as DesignProposal)}>Apply draft</Button>
						<Button size="sm" variant="ghost" onclick={() => (current = null)}>Reject</Button>
					</div>
				</div>
				<div>
					<h3 class="font-medium">Hypotheses</h3>
					<ul class="list-disc pl-5">
						{#each p.hypotheses as h, i (i)}<li><b>{h.id}</b> {h.text}</li>{/each}
					</ul>
				</div>
				<div>
					<h3 class="font-medium">Observation groups</h3>
					{#each p.observation_groups as g, i (i)}
						<div class="mt-1">
							<span class="font-medium">{g.name}</span> <Badge variant="tertiary">{g.role}</Badge>
							<div class="mt-1 flex flex-wrap gap-1">
								{#each g.candidate_pieces as piece, j (j)}<ResearchPiece {piece} />{/each}
							</div>
						</div>
					{/each}
				</div>
				<div>
					<h3 class="font-medium">Interventions</h3>
					<ul class="list-disc pl-5">
						{#each p.intervention_proposals as r, i (i)}
							<li>{r.operation} on {r.target_group} delta {r.delta}</li>
						{:else}
							<li class="text-muted-foreground">none</li>
						{/each}
					</ul>
				</div>
				{#if p.controls || p.measurement_plan}
					<div class="text-xs text-muted-foreground">
						controls: {JSON.stringify(p.controls ?? {})} · measurement: {JSON.stringify(p.measurement_plan ?? {})}
					</div>
				{/if}
				<div>
					<h3 class="font-medium">Limitations</h3>
					<ul class="list-disc pl-5">
						{#each p.limitations as l, i (i)}<li>{l}</li>{/each}
					</ul>
				</div>
			</div>
		{/if}
	</section>
</div>
