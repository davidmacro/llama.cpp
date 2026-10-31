import { ResearchApiError, ResearchService } from '$lib/services/research.service';
import type {
	CompiledPlan,
	DesignGroup,
	DesignProposal,
	DesignSpec,
	EffectiveBias,
	InterventionRule,
	Observation,
	ResearchDesign,
	ResearchEvent,
	ResearchIntervention,
	ResearchProject,
	ResearchProposal,
	ResearchQuestion,
	ResearchStatus,
	RunManifest,
	RunSummary,
	RunToken
} from '$lib/types/research';
import { groupColor, slugId } from '$lib/utils/research';
import { SvelteMap } from 'svelte/reactivity';

export type ResearchTab = 'question' | 'design' | 'interventions' | 'launch' | 'runs' | 'compare';

const FLUSH_MS = 100; // ~10 FPS render cadence for live runs

export function emptySpec(): DesignSpec {
	return {
		schema_version: 1,
		groups: [],
		watch_extra_token_ids: [],
		unknown_multi_token_policy: 'flag_and_exclude'
	};
}

export function errMsg(e: unknown): string {
	if (e instanceof ResearchApiError) return `${e.code}: ${e.message}`;
	return e instanceof Error ? e.message : String(e);
}

/**
 * One run as viewed in the inspector. SSE events are buffered in plain arrays
 * and flushed into $state.raw arrays on a fixed interval, so a fast run does
 * not trigger one reactive update per token.
 */
export class RunView {
	id: string;
	manifest = $state.raw<RunManifest | null>(null);
	tokens = $state.raw<RunToken[]>([]);
	observations = $state.raw<Observation[]>([]);
	watchIds = $state.raw<number[]>([]);
	groups = $state.raw<{ id: string; name: string }[]>([]);
	status = $state('loading');
	counters = $state({ captured: 0, skipped: 0, dropped: 0 });
	text = $state('');
	error = $state<string | null>(null);
	live = $state(false);
	eventsPerSec = $state(0);

	#pendingTokens: RunToken[] = [];
	#pendingObs: Observation[] = [];
	#lastSeq = 0;
	#abort: AbortController | null = null;
	#timer: ReturnType<typeof setInterval> | null = null;
	#evCount = 0;
	#evWindowStart = 0;

	constructor(id: string) {
		this.id = id;
	}

	async open(): Promise<void> {
		try {
			const m = await ResearchService.getRun(this.id);
			this.manifest = m;
			this.status = m.status;
			this.text = m.text ?? '';
			this.counters = { captured: m.captured, skipped: m.skipped, dropped: m.dropped };
			if (m.compiled) {
				this.watchIds = m.compiled.watch_ids;
				this.groups = m.compiled.groups.map((g) => ({ id: g.id, name: g.name }));
			}
			if (m.status === 'running' || m.status === 'queued') {
				this.follow();
			} else {
				await this.loadStored();
			}
		} catch (e) {
			this.error = errMsg(e);
			this.status = 'error';
		}
	}

	/** Load persisted tokens + observations (works after server restart). */
	async loadStored(): Promise<void> {
		const [t, o] = await Promise.all([
			ResearchService.runTokens(this.id),
			ResearchService.runObservations(this.id)
		]);
		this.tokens = t.tokens;
		this.observations = o.observations;
		if (o.watch_ids?.length) this.watchIds = o.watch_ids;
		if (o.groups?.length) this.groups = o.groups;
	}

	follow(): void {
		if (this.#abort) return;
		this.live = true;
		this.#abort = new AbortController();
		this.#evWindowStart = performance.now();
		this.#timer = setInterval(() => this.#flush(), FLUSH_MS);
		ResearchService.streamEvents(this.id, this.#lastSeq, (ev) => this.#onEvent(ev), this.#abort.signal)
			.catch((e) => {
				if ((e as Error).name !== 'AbortError') this.error = errMsg(e);
			})
			.finally(() => {
				this.#flush();
				this.#stopTimer();
				this.live = false;
				this.#abort = null;
			});
	}

	#onEvent(ev: ResearchEvent): void {
		if (ev.seq > this.#lastSeq) this.#lastSeq = ev.seq;
		this.#evCount++;
		switch (ev.event) {
			case 'research.run_started':
				this.#pendingStatus = 'running';
				break;
			case 'research.token_accepted':
				this.#pendingTokens.push(ev.data);
				break;
			case 'research.observation':
				this.#pendingObs.push(ev.data);
				break;
			case 'research.run_finished':
				this.#pendingStatus = ev.data.status;
				this.#pendingFinish = ev.data;
				break;
		}
	}

	#pendingStatus: string | null = null;
	#pendingFinish: { captured: number; skipped: number; dropped: number; text: string } | null =
		null;

	#flush(): void {
		if (this.#pendingTokens.length) {
			this.tokens = this.tokens.concat(this.#pendingTokens);
			this.#pendingTokens = [];
		}
		if (this.#pendingObs.length) {
			this.observations = this.observations.concat(this.#pendingObs);
			// live counters: captured is exact; skipped is derived from tokens seen
			this.#pendingObs = [];
		}
		if (this.#pendingStatus) {
			this.status = this.#pendingStatus;
			this.#pendingStatus = null;
		}
		if (this.#pendingFinish) {
			const f = this.#pendingFinish;
			this.counters = { captured: f.captured, skipped: f.skipped, dropped: f.dropped };
			this.text = f.text;
			this.#pendingFinish = null;
		} else if (this.live) {
			this.counters = {
				captured: this.observations.length,
				skipped: Math.max(0, this.tokens.length - this.observations.length),
				dropped: this.counters.dropped
			};
		}
		const now = performance.now();
		if (now - this.#evWindowStart >= 1000) {
			this.eventsPerSec = Math.round((this.#evCount * 1000) / (now - this.#evWindowStart));
			this.#evCount = 0;
			this.#evWindowStart = now;
		}
	}

	#stopTimer(): void {
		if (this.#timer) clearInterval(this.#timer);
		this.#timer = null;
	}

	stop(): void {
		this.#abort?.abort();
		this.#stopTimer();
	}
}

class ResearchStore {
	status = $state.raw<ResearchStatus | null>(null);
	statusError = $state<string | null>(null);

	tab = $state<ResearchTab>('question');

	projects = $state.raw<ResearchProject[]>([]);
	project = $state.raw<ResearchProject | null>(null);
	question = $state.raw<ResearchQuestion | null>(null);

	designs = $state.raw<ResearchDesign[]>([]);
	interventions = $state.raw<ResearchIntervention[]>([]);
	proposals = $state.raw<ResearchProposal[]>([]);
	runs = $state.raw<RunSummary[]>([]);

	/** Draft spec edited in the hypertoken builder (deep state on purpose). */
	draft = $state<DesignSpec>(emptySpec());
	preview = $state.raw<CompiledPlan | null>(null);
	designId = $state<string | null>(null);

	ruleDraft = $state<InterventionRule[]>([]);
	interventionId = $state<string | null>(null);
	effectiveBias = $state.raw<EffectiveBias | null>(null);

	/** Opened runs (inspector / compare). */
	views = new SvelteMap<string, RunView>();
	selectedRunId = $state<string | null>(null);
	compareIds = $state<[string | null, string | null]>([null, null]);

	busy = $state(false);
	error = $state<string | null>(null);

	get design(): ResearchDesign | null {
		return this.designs.find((d) => d.id === this.designId) ?? null;
	}

	async run<T>(fn: () => Promise<T>): Promise<T | undefined> {
		this.busy = true;
		this.error = null;
		try {
			return await fn();
		} catch (e) {
			this.error = errMsg(e);
			return undefined;
		} finally {
			this.busy = false;
		}
	}

	async init(): Promise<void> {
		try {
			this.status = await ResearchService.status();
			this.statusError = null;
		} catch (e) {
			this.status = { enabled: false };
			this.statusError = errMsg(e);
			return;
		}
		if (this.status.enabled) await this.loadProjects();
	}

	async loadProjects(): Promise<void> {
		await this.run(async () => {
			this.projects = (await ResearchService.listProjects()).projects;
		});
	}

	async createProject(title: string, description: string): Promise<void> {
		await this.run(async () => {
			const p = await ResearchService.createProject(title, description || undefined);
			this.projects = [...this.projects, p];
			await this.selectProject(p.id);
		});
	}

	async selectProject(id: string): Promise<void> {
		await this.run(async () => {
			this.project = await ResearchService.getProject(id);
			this.question = null;
		});
	}

	async createQuestion(q: Partial<ResearchQuestion>): Promise<void> {
		if (!this.project) return;
		const projectId = this.project.id;
		await this.run(async () => {
			const created = await ResearchService.createQuestion(projectId, q);
			this.project = await ResearchService.getProject(projectId);
			await this.selectQuestion(created.id);
		});
	}

	async saveQuestion(q: Partial<ResearchQuestion>): Promise<void> {
		if (!this.question) return;
		const id = this.question.id;
		await this.run(async () => {
			this.question = await ResearchService.patchQuestion(id, q);
			if (this.project) this.project = await ResearchService.getProject(this.project.id);
		});
	}

	async selectQuestion(id: string): Promise<void> {
		await this.run(async () => {
			this.question = await ResearchService.getQuestion(id);
			this.designId = null;
			this.interventionId = null;
			this.draft = emptySpec();
			this.ruleDraft = [];
			this.preview = null;
			this.effectiveBias = null;
			await this.refreshQuestionData();
			const latest = this.designs[this.designs.length - 1];
			if (latest) this.loadDesignIntoDraft(latest);
		});
	}

	async refreshQuestionData(): Promise<void> {
		if (!this.question) return;
		const id = this.question.id;
		const [d, i, p, r] = await Promise.all([
			ResearchService.listDesigns(id),
			ResearchService.listInterventions(id),
			ResearchService.listProposals(id).catch(() => ({ proposals: [] })),
			ResearchService.listRuns(id)
		]);
		this.designs = d.designs;
		this.interventions = i.interventions;
		this.proposals = p.proposals;
		this.runs = r.runs;
	}

	async refreshRuns(): Promise<void> {
		if (!this.question) return;
		const id = this.question.id;
		await this.run(async () => {
			this.runs = (await ResearchService.listRuns(id)).runs;
		});
	}

	loadDesignIntoDraft(d: ResearchDesign): void {
		this.designId = d.id;
		const spec = structuredClone($state.snapshot(d.spec)) as DesignSpec;
		spec.groups.forEach((g, i) => (g.color = g.color ?? groupColor(i)));
		spec.watch_extra_token_ids ??= [];
		spec.unknown_multi_token_policy ??= 'flag_and_exclude';
		this.draft = spec;
		this.preview = d.compiled ?? null;
	}

	addGroup(name = 'GROUP', role: DesignGroup['role'] = 'target'): DesignGroup {
		const taken = new Set(this.draft.groups.map((g) => g.id));
		const g: DesignGroup = {
			id: slugId(name, taken),
			name,
			role,
			selectors: [],
			color: groupColor(this.draft.groups.length)
		};
		this.draft.groups.push(g);
		return g;
	}

	/** Fill builders from an untrusted proposal. Only text pieces, never IDs. */
	applyProposal(p: DesignProposal): void {
		this.draft = emptySpec();
		this.designId = null;
		this.preview = null;
		const nameToId = new Map<string, string>();
		for (const og of p.observation_groups) {
			const g = this.addGroup(og.name, og.role);
			const pieces = og.candidate_pieces.filter((s) => typeof s === 'string' && s.length > 0);
			if (pieces.length) g.selectors.push({ kind: 'variant_list', texts: pieces });
			nameToId.set(og.name, g.id);
		}
		this.ruleDraft = p.intervention_proposals
			.filter((ip) => nameToId.has(ip.target_group))
			.map((ip, i) => ({
				id: `r${i + 1}`,
				target_group_id: nameToId.get(ip.target_group)!,
				operation: ip.operation,
				delta: Number(ip.delta) || 0,
				enabled: true
			}));
		this.interventionId = null;
		this.effectiveBias = null;
		this.tab = 'design';
	}

	async compilePreview(): Promise<void> {
		const spec = $state.snapshot(this.draft) as DesignSpec;
		await this.run(async () => {
			this.preview = await ResearchService.compilePreview(spec);
		});
	}

	async saveDesign(): Promise<ResearchDesign | undefined> {
		if (!this.question) return;
		const qid = this.question.id;
		const spec = $state.snapshot(this.draft) as DesignSpec;
		return this.run(async () => {
			const d = await ResearchService.createDesign(qid, spec);
			this.designs = (await ResearchService.listDesigns(qid)).designs;
			this.designId = d.id;
			return d;
		});
	}

	async compileAndApprove(): Promise<void> {
		const id = this.designId;
		if (!id || !this.question) return;
		const qid = this.question.id;
		await this.run(async () => {
			this.preview = await ResearchService.compileDesign(id);
			if (!this.preview.ok) throw new Error('compile reported errors; fix before approving');
			await ResearchService.approveDesign(id);
			this.designs = (await ResearchService.listDesigns(qid)).designs;
		});
	}

	async compileOnly(): Promise<void> {
		const id = this.designId;
		if (!id || !this.question) return;
		const qid = this.question.id;
		await this.run(async () => {
			this.preview = await ResearchService.compileDesign(id);
			this.designs = (await ResearchService.listDesigns(qid)).designs;
		});
	}

	async saveIntervention(approve: boolean): Promise<void> {
		if (!this.question || !this.designId) {
			this.error = 'Select or save a design first.';
			return;
		}
		const qid = this.question.id;
		const did = this.designId;
		const rules = $state.snapshot(this.ruleDraft) as InterventionRule[];
		await this.run(async () => {
			let iv = await ResearchService.createIntervention(qid, did, rules);
			if (approve) iv = await ResearchService.approveIntervention(iv.id);
			this.interventions = (await ResearchService.listInterventions(qid)).interventions;
			this.interventionId = iv.id;
			this.effectiveBias = await ResearchService.effectiveBias(iv.id);
		});
	}

	async selectIntervention(id: string): Promise<void> {
		const iv = this.interventions.find((x) => x.id === id);
		if (!iv) return;
		this.interventionId = id;
		this.ruleDraft = structuredClone(iv.rules);
		await this.run(async () => {
			this.effectiveBias = await ResearchService.effectiveBias(id);
		});
	}

	openRun(id: string): RunView {
		let v = this.views.get(id);
		if (!v) {
			v = new RunView(id);
			this.views.set(id, v);
			void v.open();
		}
		return v;
	}

	closeRun(id: string): void {
		this.views.get(id)?.stop();
		this.views.delete(id);
	}
}

export const researchStore = new ResearchStore();
