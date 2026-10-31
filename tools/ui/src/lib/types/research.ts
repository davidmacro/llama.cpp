// Research module (POC) types. Mirrors tools/server/research/README.md.

export interface ResearchStatus {
	enabled: boolean;
	model_id?: string;
	n_vocab?: number;
	tokenizer_fingerprint?: string;
	schema_version?: number;
}

export interface ResearchProject {
	id: string;
	title: string;
	description?: string;
	created_at?: string;
	questions?: ResearchQuestion[];
}

export interface ResearchQuestion {
	id: string;
	project_id?: string;
	question_text: string;
	hypothesis?: string;
	notes?: string;
	status?: string;
	created_at?: string;
}

export type GroupRole = 'target' | 'competitor' | 'control';

export type Selector =
	| { kind: 'exact_token_piece'; text: string }
	| { kind: 'variant_list'; texts: string[] }
	| { kind: 'token_ids'; ids: number[] };

export interface DesignGroup {
	id: string;
	name: string;
	role: GroupRole;
	selectors: Selector[];
	/** UI only; stripped before sending. */
	color?: string;
	/** UI only; stable key while the id follows the name. */
	uid?: string;
	/** Token ids added as "first token of a multi-token word" (approximate members). */
	approx_ids?: number[];
}

export interface DesignSpec {
	schema_version: 1;
	groups: DesignGroup[];
	watch_extra_token_ids: number[];
	unknown_multi_token_policy: 'flag_and_exclude';
}

export interface ResearchDesign {
	id: string;
	question_id: string;
	revision?: number;
	state: 'draft' | 'compiled' | 'approved' | string;
	spec: DesignSpec;
	compiled_id?: string | null;
	compiled?: CompiledPlan;
	created_at?: string;
}

export interface TokenInfo {
	id: number;
	piece: string;
	piece_escaped?: string;
	piece_b64?: string;
	type?: string;
	special?: boolean;
}

export type CandidateStatus =
	| 'accepted_single_token'
	| 'multi_token'
	| 'not_found'
	| 'special_token'
	| 'ambiguous_duplicate_piece';

export interface CompiledCandidate {
	group_id: string;
	selector_kind: string;
	value: string | number;
	status: CandidateStatus | string;
	token_ids: number[];
	pieces: string[];
}

export interface CompiledGroup {
	id: string;
	name: string;
	role: string;
	members: number[];
	warnings: string[];
}

export interface CompiledPlan {
	compiled_id: string | null;
	tokenizer_fingerprint: string;
	model_id: string;
	watch_ids: number[];
	groups: CompiledGroup[];
	candidates: CompiledCandidate[];
	overlaps: { token_id: number; group_ids: string[] }[];
	warnings: string[];
	fingerprint: string;
	ok: boolean;
}

export type RuleOperation = 'add_logit_bias' | 'exclude';

export interface InterventionRule {
	id: string;
	target_group_id: string;
	operation: RuleOperation;
	delta: number;
	enabled: boolean;
}

export interface ResearchIntervention {
	id: string;
	question_id?: string;
	design_id: string;
	rules: InterventionRule[];
	state?: string;
	created_at?: string;
}

export interface EffectiveBiasEntry {
	token_id: number;
	piece: string;
	delta: number;
	excluded: boolean;
	rules: string[];
}

export interface EffectiveBias {
	entries: EffectiveBiasEntry[];
	warnings: string[];
}

export interface ResearchProposal {
	id: string;
	question_id: string;
	model?: string;
	prompt?: string;
	raw_output?: string;
	proposal: DesignProposal;
	created_at?: string;
}

/** Suggestion schema of design doc 08. Untrusted LLM output. */
export interface DesignProposal {
	schema_version: number;
	research_question?: string;
	hypotheses: { id: string; text: string }[];
	observation_groups: { name: string; role: GroupRole; candidate_pieces: string[] }[];
	intervention_proposals: { target_group: string; operation: RuleOperation; delta: number }[];
	controls?: { include_unmodified?: boolean; matched_prompts?: boolean; repeat_seeds?: number };
	measurement_plan?: { capture_every_n?: number; required_stages?: string[] };
	limitations: string[];
}

export type CollectionMode = 'raw_watch' | 'watch_normalized';

export interface BatchRequest {
	question_id: string;
	design_id: string;
	arms: { name: string; intervention_id: string | null }[];
	prompt: string;
	chat: boolean;
	seeds: number[];
	n_predict: number;
	sampling: { temperature: number; top_k: number; top_p: number };
	collection: { every_n: number; mode: CollectionMode; top_k: number };
}

export interface BatchCreated {
	batch_id: string;
	run_ids: string[];
	status: string;
}

export type RunStatus = 'queued' | 'running' | 'completed' | 'failed' | 'cancelled' | 'interrupted';

export interface RunSummary {
	id: string;
	batch_id: string;
	question_id: string;
	arm: string;
	intervention_id: string | null;
	seed: number;
	status: RunStatus | string;
	n_tokens: number;
	captured: number;
	skipped: number;
	dropped: number;
	text: string;
	error?: string | null;
	created_at?: string;
	started_at?: string;
	ended_at?: string;
}

export interface RunManifest extends RunSummary {
	prompt?: string;
	chat?: boolean;
	n_predict?: number;
	design_id?: string;
	rendered_prompt?: string;
	sampling?: Record<string, unknown>;
	collection?: { every_n: number; mode: CollectionMode; top_k: number };
	compiled?: CompiledPlan;
	effective_bias?: { token_id: number; delta: number; excluded: boolean }[];
	model_id?: string;
	tokenizer_fingerprint?: string;
	llama_build?: string;
	stage_semantics?: unknown;
	plan_hash?: string;
}

export interface RunToken {
	generation_index: number;
	token_id: number;
	piece: string;
	piece_b64?: string;
	context_position?: number;
}

export interface GroupMetric {
	id: string;
	raw_lse: number | null;
	adj_lse: number | null;
	raw_rel: number | null;
	adj_rel: number | null;
	raw_abs: number | null;
	adj_abs: number | null;
}

export interface Observation {
	generation_index: number;
	context_position?: number;
	selected_token_id?: number;
	raw: (number | null)[];
	adjusted: (number | null)[];
	raw_log_z: number | null;
	adjusted_log_z: number | null;
	groups: GroupMetric[];
	top_k?: { id: number; logit: number }[];
	capture_state?: string;
}

export interface RunObservations {
	watch_ids: number[];
	groups: { id: string; name: string }[];
	observations: Observation[];
}

export interface RunFinished {
	status: string;
	n_tokens: number;
	captured: number;
	skipped: number;
	dropped: number;
	text: string;
}

export type ResearchEvent =
	| { event: 'research.run_started'; seq: number; data: { status: string } }
	| { event: 'research.token_accepted'; seq: number; data: RunToken }
	| { event: 'research.observation'; seq: number; data: Observation }
	| { event: 'research.run_finished'; seq: number; data: RunFinished };

export interface CounterfactualResult {
	raw: { groups: Partial<GroupMetric>[] | Record<string, unknown> };
	adjusted: { groups: Partial<GroupMetric>[] | Record<string, unknown> };
}

export type MetricKey = 'lse' | 'rel' | 'abs';

/** Compact per-run result used by the batch matrix (first captured position). */
export interface RunDigest {
	/** post_intervention top group */
	groupName: string | null;
	groupId: string | null;
	mass: number | null;
	massKind: 'abs' | 'rel';
	/** raw (pre-intervention) top group */
	rawGroupName: string | null;
	rawGroupId: string | null;
	rawMass: number | null;
	/** post_intervention mass in no watched group (null without log Z) */
	unwatched: number | null;
	/** selected token at the first captured position is in no group */
	outside: boolean;
	/** a position was captured */
	captured: boolean;
}
