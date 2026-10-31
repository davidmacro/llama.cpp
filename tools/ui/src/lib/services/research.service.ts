import { base } from '$app/paths';
import { getAuthHeaders, getJsonHeaders } from '$lib/utils/api-headers';
import type {
	BatchCreated,
	BatchRequest,
	CompiledPlan,
	CounterfactualResult,
	DesignProposal,
	DesignSpec,
	EffectiveBias,
	InterventionRule,
	ResearchDesign,
	ResearchEvent,
	ResearchIntervention,
	ResearchProject,
	ResearchProposal,
	ResearchQuestion,
	ResearchStatus,
	RunManifest,
	RunObservations,
	RunSummary,
	RunToken,
	TokenInfo
} from '$lib/types/research';

const PREFIX = '/api/research/v1';

/** Error carrying the server's machine-readable code (e.g. RESEARCH_DISABLED). */
export class ResearchApiError extends Error {
	status: number;
	code: string;

	constructor(message: string, status: number, code: string) {
		super(message);
		this.name = 'ResearchApiError';
		this.status = status;
		this.code = code;
	}
}

function url(path: string): string {
	return `${base}${path}`;
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
	let res: Response;
	try {
		res = await fetch(url(PREFIX + path), { ...init, headers: getJsonHeaders() });
	} catch (e) {
		throw new ResearchApiError(`network error: ${(e as Error).message}`, 0, 'NETWORK');
	}
	if (!res.ok) {
		let code = `HTTP_${res.status}`;
		let message = res.statusText || code;
		try {
			const body = await res.json();
			if (body?.error?.code) code = body.error.code;
			if (body?.error?.message) message = body.error.message;
			else if (typeof body?.error === 'string') message = body.error;
		} catch {
			// non-JSON error body
		}
		throw new ResearchApiError(message, res.status, code);
	}
	return (await res.json()) as T;
}

const post = <T>(path: string, body?: unknown) =>
	request<T>(path, { method: 'POST', body: body === undefined ? undefined : JSON.stringify(body) });

function qs(params: Record<string, string | number | undefined | null>): string {
	const sp = new URLSearchParams();
	for (const [k, v] of Object.entries(params)) {
		if (v !== undefined && v !== null && v !== '') sp.set(k, String(v));
	}
	const s = sp.toString();
	return s ? `?${s}` : '';
}

/** Strip UI-only fields (group color) before sending a spec to the server. */
export function cleanSpec(spec: DesignSpec): DesignSpec {
	return {
		schema_version: 1,
		groups: spec.groups.map((g) => ({
			id: g.id,
			name: g.name,
			role: g.role,
			// drop empty variant lines left over from editing
			selectors: g.selectors.map((s) =>
				s.kind === 'variant_list' ? { kind: s.kind, texts: s.texts.filter((t) => t.length > 0) } : s
			)
		})),
		watch_extra_token_ids: spec.watch_extra_token_ids,
		unknown_multi_token_policy: spec.unknown_multi_token_policy
	};
}

export const ResearchService = {
	status: () => request<ResearchStatus>('/status'),

	listProjects: () => request<{ projects: ResearchProject[] }>('/projects'),
	createProject: (title: string, description?: string) =>
		post<ResearchProject>('/projects', { title, description }),
	getProject: (id: string) => request<ResearchProject>(`/projects/${id}`),

	createQuestion: (projectId: string, q: Partial<ResearchQuestion>) =>
		post<ResearchQuestion>(`/projects/${projectId}/questions`, q),
	getQuestion: (id: string) => request<ResearchQuestion>(`/questions/${id}`),
	patchQuestion: (id: string, q: Partial<ResearchQuestion>) =>
		request<ResearchQuestion>(`/questions/${id}`, { method: 'PATCH', body: JSON.stringify(q) }),

	listDesigns: (questionId: string) =>
		request<{ designs: ResearchDesign[] }>(`/questions/${questionId}/designs`),
	createDesign: (questionId: string, spec: DesignSpec) =>
		post<ResearchDesign>(`/questions/${questionId}/designs`, { spec: cleanSpec(spec) }),
	getDesign: (id: string) => request<ResearchDesign>(`/designs/${id}`),
	compilePreview: (spec: DesignSpec) =>
		post<CompiledPlan>('/compile-preview', { spec: cleanSpec(spec) }),
	compileDesign: (id: string) => post<CompiledPlan>(`/designs/${id}/compile`),
	approveDesign: (id: string) => post<ResearchDesign>(`/designs/${id}/approve`),

	listInterventions: (questionId: string) =>
		request<{ interventions: ResearchIntervention[] }>(`/questions/${questionId}/interventions`),
	createIntervention: (questionId: string, designId: string, rules: InterventionRule[]) =>
		post<ResearchIntervention>(`/questions/${questionId}/interventions`, {
			design_id: designId,
			rules
		}),
	approveIntervention: (id: string) => post<ResearchIntervention>(`/interventions/${id}/approve`),
	effectiveBias: (id: string) => request<EffectiveBias>(`/interventions/${id}/effective-bias`),

	vocab: (query: string, mode: 'substring' | 'exact' | 'prefix', limit = 50, offset = 0) =>
		request<{ n_vocab: number; tokens: TokenInfo[] }>(
			`/vocab${qs({ query, mode, limit, offset, include_special: 0 })}`
		),
	token: (id: number) => request<TokenInfo>(`/tokens/${id}`),
	tokenizePreview: (text: string) =>
		post<{ tokens: TokenInfo[] }>('/tokenize-preview', {
			text,
			add_special: false,
			parse_special: false
		}),

	createProposal: (body: {
		question_id: string;
		model: string;
		prompt: string;
		raw_output: string;
		proposal: DesignProposal;
	}) => post<ResearchProposal>('/proposals', body),
	listProposals: (questionId: string) =>
		request<{ proposals: ResearchProposal[] }>(`/questions/${questionId}/proposals`),

	createBatch: (req: BatchRequest) => post<BatchCreated>('/batches', req),
	listRuns: (questionId?: string, batchId?: string) =>
		request<{ runs: RunSummary[] }>(`/runs${qs({ question_id: questionId, batch_id: batchId })}`),
	getRun: (id: string) => request<RunManifest>(`/runs/${id}`),
	runTokens: (id: string) => request<{ tokens: RunToken[] }>(`/runs/${id}/tokens`),
	runObservations: (id: string) => request<RunObservations>(`/runs/${id}/observations`),
	cancelRun: (id: string) => post<RunSummary>(`/runs/${id}/cancel`),
	counterfactual: (runId: string, generationIndex: number, interventionId: string) =>
		post<CounterfactualResult>('/counterfactual', {
			run_id: runId,
			generation_index: generationIndex,
			intervention_id: interventionId
		}),

	/** Downloads the export through fetch so the API key header is honored. */
	async exportRun(id: string, format: 'jsonl' | 'csv'): Promise<void> {
		const res = await fetch(url(`${PREFIX}/runs/${id}/export?format=${format}`), {
			headers: getAuthHeaders()
		});
		if (!res.ok) throw new ResearchApiError(`export failed (${res.status})`, res.status, 'EXPORT');
		const blob = await res.blob();
		const a = document.createElement('a');
		a.href = URL.createObjectURL(blob);
		a.download = `${id}.${format}`;
		a.click();
		setTimeout(() => URL.revokeObjectURL(a.href), 1000);
	},

	/**
	 * Follows /runs/{id}/events with fetch streaming (EventSource cannot send the
	 * Authorization header). Calls onEvent per parsed record; resolves when the
	 * stream ends or the signal aborts.
	 */
	async streamEvents(
		id: string,
		after: number,
		onEvent: (ev: ResearchEvent) => void,
		signal: AbortSignal
	): Promise<void> {
		const res = await fetch(url(`${PREFIX}/runs/${id}/events?after=${after}`), {
			headers: { ...getAuthHeaders(), Accept: 'text/event-stream' },
			signal
		});
		if (!res.ok || !res.body) {
			throw new ResearchApiError(`events failed (${res.status})`, res.status, 'EVENTS');
		}
		const reader = res.body.getReader();
		const decoder = new TextDecoder();
		let buf = '';
		try {
			for (;;) {
				const { done, value } = await reader.read();
				if (done) break;
				buf += decoder.decode(value, { stream: true }).replace(/\r\n/g, '\n');
				let idx: number;
				while ((idx = buf.indexOf('\n\n')) >= 0) {
					const rec = buf.slice(0, idx);
					buf = buf.slice(idx + 2);
					const ev = parseRecord(rec);
					if (ev) onEvent(ev);
				}
			}
		} finally {
			reader.releaseLock();
		}
	}
};

function parseRecord(rec: string): ResearchEvent | null {
	let name = 'message';
	let id = -1;
	const data: string[] = [];
	for (const line of rec.split('\n')) {
		if (line.startsWith(':')) continue;
		const c = line.indexOf(':');
		const field = c < 0 ? line : line.slice(0, c);
		let value = c < 0 ? '' : line.slice(c + 1);
		if (value.startsWith(' ')) value = value.slice(1);
		if (field === 'event') name = value;
		else if (field === 'id') id = Number(value);
		else if (field === 'data') data.push(value);
	}
	if (data.length === 0) return null;
	try {
		const parsed = JSON.parse(data.join('\n'));
		// tolerate servers that put the event name inside data only
		if (name === 'message' && typeof parsed?.event === 'string') name = parsed.event;
		const seq = Number.isFinite(id) && id >= 0 ? id : Number(parsed?.event_seq ?? -1);
		return { event: name, seq, data: parsed } as ResearchEvent;
	} catch {
		return null;
	}
}

/** Calls the server's own OpenAI-compatible endpoint (non-streaming). */
export async function chatCompletionJson(
	messages: { role: string; content: string }[],
	schema: Record<string, unknown> | null
): Promise<{ model: string; content: string }> {
	const body: Record<string, unknown> = {
		messages,
		temperature: 0.3,
		stream: false,
		max_tokens: 2048,
		chat_template_kwargs: { enable_thinking: false },
		response_format: schema
			? { type: 'json_schema', json_schema: { name: 'design_proposal', schema, strict: true } }
			: { type: 'json_object' }
	};
	const res = await fetch(url('/v1/chat/completions'), {
		method: 'POST',
		headers: getJsonHeaders(),
		body: JSON.stringify(body)
	});
	if (!res.ok) {
		let msg = `${res.status}`;
		try {
			const j = await res.json();
			msg = j?.error?.message ?? msg;
		} catch {
			// ignore
		}
		throw new Error(`chat completion failed: ${msg}`);
	}
	const j = await res.json();
	return { model: j?.model ?? 'unknown', content: j?.choices?.[0]?.message?.content ?? '' };
}
