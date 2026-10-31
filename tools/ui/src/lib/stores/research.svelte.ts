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
	RunDigest,
	RunManifest,
	RunSummary,
	RunToken,
	TokenInfo
} from '$lib/types/research';
import { groupColor, runTime, slugId, topGroup, unwatchedMass } from '$lib/utils/research';
import { SvelteMap, SvelteSet } from 'svelte/reactivity';

export type ResearchTab = 'question' | 'design' | 'interventions' | 'launch' | 'runs' | 'compare';

const FLUSH_MS = 100; // ~10 FPS render cadence for live runs
const POLL_MS = 1500; // run list refresh while runs are queued/running

const ACTIVE = new Set(['queued', 'running']);
const HIDDEN_KEY = 'research.hiddenInterventions';

export interface BatchInfo {
	id: string;
	time: string;
	arms: string[];
	seeds: number[];
	runs: RunSummary[];
}

function loadHidden(): string[] {
	try {
		const v = JSON.parse(localStorage.getItem(HIDDEN_KEY) ?? '[]');
		return Array.isArray(v) ? v.filter((x) => typeof x === 'string') : [];
	} catch {
		return [];
	}
}

let uidSeq = 0;
const nextUid = () => `g${++uidSeq}`;

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

	onFinish: (() => void) | null = null;

	constructor(id: string, onFinish?: () => void) {
		this.id = id;
		this.onFinish = onFinish ?? null;
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
			this.onFinish?.();
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

	/** Batch shown in the results matrix (null = latest). */
	batchPick = $state<string | null>(null);
	/** Matrix digests per finished run (runs are immutable once finished). */
	digests = new SvelteMap<string, RunDigest>();
	#digestPending = new Set<string>();

	/** Interventions hidden in the UI only (localStorage). */
	hiddenIvs = new SvelteSet<string>(typeof localStorage === 'undefined' ? [] : loadHidden());

	/** Opened runs (inspector / compare). */
	views = new SvelteMap<string, RunView>();
	selectedRunId = $state<string | null>(null);
	compareIds = $state<[string | null, string | null]>([null, null]);

	busy = $state(false);
	error = $state<string | null>(null);

	/** Token piece cache shared by all views (fetched once per id). */
	tokenCache = new SvelteMap<number, TokenInfo>();
	#tokenPending = new Set<number>();
	#pollTimer: ReturnType<typeof setTimeout> | null = null;

	/** Display name of a group id (draft first, then saved designs). */
	groupName(id: string): string {
		const g = this.draft.groups.find((x) => x.id === id);
		if (g) return g.name;
		for (const d of this.designs) {
			const dg = d.spec.groups.find((x) => x.id === id);
			if (dg) return dg.name;
		}
		return id;
	}

	ensureTokens(ids: number[]): void {
		const missing = ids.filter((id) => !this.tokenCache.has(id) && !this.#tokenPending.has(id));
		if (missing.length === 0) return;
		for (const id of missing) this.#tokenPending.add(id);
		for (const id of missing)
			ResearchService.token(id)
				.then((t) => this.tokenCache.set(id, t))
				.catch(() => null)
				.finally(() => this.#tokenPending.delete(id));
	}

	/** Poll /runs while any listed run is queued/running; stops by itself. */
	pollRuns(): void {
		if (this.#pollTimer || !this.runs.some((r) => ACTIVE.has(r.status))) return;
		this.#pollTimer = setTimeout(() => {
			this.#pollTimer = null;
			void this.syncRuns();
		}, POLL_MS);
	}

	/** Silent run list refresh, then keep polling if needed. */
	async syncRuns(): Promise<void> {
		const qid = this.question?.id;
		if (!qid) return;
		try {
			const runs = (await ResearchService.listRuns(qid)).runs;
			if (this.question?.id === qid) this.runs = runs;
		} catch {
			// transient; retried on the next poll
		}
		this.pollRuns();
	}

	/** A = control, B = first treatment with the same seed (only when empty). */
	prefillCompare(): void {
		if (this.compareIds[0] || this.compareIds[1]) return;
		const control = this.runs.find((r) => !r.intervention_id);
		if (!control) return;
		const treat =
			this.runs.find(
				(r) => r.intervention_id && r.seed === control.seed && r.batch_id === control.batch_id
			) ?? this.runs.find((r) => r.intervention_id && r.seed === control.seed);
		this.openRun(control.id);
		if (treat) this.openRun(treat.id);
		this.compareIds = [control.id, treat?.id ?? null];
	}

	/** Batches derived from the run list, newest first. */
	batches = $derived.by((): BatchInfo[] => {
		const map = new Map<string, BatchInfo>();
		for (const r of this.runs) {
			let b = map.get(r.batch_id);
			if (!b) {
				b = { id: r.batch_id, time: runTime(r), arms: [], seeds: [], runs: [] };
				map.set(r.batch_id, b);
			}
			b.runs.push(r);
			const t = runTime(r);
			if (t && (!b.time || t < b.time)) b.time = t;
			if (!b.arms.includes(r.arm)) b.arms.push(r.arm);
			if (!b.seeds.includes(r.seed)) b.seeds.push(r.seed);
		}
		for (const b of map.values()) {
			b.seeds.sort((x, y) => x - y);
			// control first, then in run order
			b.arms.sort((x, y) => (x === 'control' ? -1 : y === 'control' ? 1 : 0));
		}
		return [...map.values()].sort((a, b) => (a.time < b.time ? 1 : a.time > b.time ? -1 : 0));
	});

	batch = $derived(this.batches.find((b) => b.id === this.batchPick) ?? this.batches[0] ?? null);

	/** Fetch observations once per finished run and keep the first-position digest. */
	ensureDigests(runs: RunSummary[]): void {
		for (const r of runs) {
			if (ACTIVE.has(r.status) || this.digests.has(r.id) || this.#digestPending.has(r.id)) continue;
			this.#digestPending.add(r.id);
			Promise.all([
				ResearchService.runObservations(r.id),
				ResearchService.getRun(r.id).catch(() => null)
			])
				.then(([o, m]) => {
					const first = o.observations[0];
					const top = topGroup(first);
					const rawTop = topGroup(first, 'raw');
					const name = (id: string | undefined) =>
						id === undefined ? null : (o.groups.find((g) => g.id === id)?.name ?? id);
					const members = new Set<number>();
					for (const g of m?.compiled?.groups ?? []) for (const id of g.members) members.add(id);
					this.digests.set(r.id, {
						groupId: top?.id ?? null,
						groupName: name(top?.id),
						mass: top?.mass ?? null,
						massKind: top?.kind ?? 'rel',
						rawGroupId: rawTop?.id ?? null,
						rawGroupName: name(rawTop?.id),
						rawMass: rawTop?.mass ?? null,
						unwatched: unwatchedMass(first, 'adj'),
						outside:
							!!first &&
							first.selected_token_id !== undefined &&
							members.size > 0 &&
							!members.has(first.selected_token_id),
						captured: !!first
					});
				})
				.catch(() => null)
				.finally(() => this.#digestPending.delete(r.id));
		}
	}

	toggleHidden(id: string): void {
		if (this.hiddenIvs.has(id)) this.hiddenIvs.delete(id);
		else this.hiddenIvs.add(id);
		try {
			localStorage.setItem(HIDDEN_KEY, JSON.stringify([...this.hiddenIvs]));
		} catch {
			// storage unavailable: hiding lasts for this session only
		}
	}

	/** Inspector action: add a token id to a draft group and open the builder. */
	addTokenToGroup(groupId: string, tokenId: number): void {
		const g = this.draft.groups.find((x) => x.id === groupId);
		if (!g) return;
		const sel = g.selectors.find((s) => s.kind === 'token_ids');
		if (!sel) g.selectors.push({ kind: 'token_ids', ids: [tokenId] });
		else if (sel.kind === 'token_ids' && !sel.ids.includes(tokenId)) sel.ids.push(tokenId);
		this.tab = 'design';
		void this.compilePreview();
	}

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
			this.batchPick = null;
			await this.refreshQuestionData();
			const latest = this.designs[this.designs.length - 1];
			if (latest) this.loadDesignIntoDraft(latest);
			const iv = this.interventions.filter((i) => i.design_id === this.designId && !this.hiddenIvs.has(i.id)).at(-1);
			if (iv) {
				this.interventionId = iv.id;
				this.effectiveBias = await ResearchService.effectiveBias(iv.id).catch(() => null);
			}
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
		this.pollRuns();
	}

	async refreshRuns(): Promise<void> {
		if (!this.question) return;
		const id = this.question.id;
		await this.run(async () => {
			this.runs = (await ResearchService.listRuns(id)).runs;
		});
		this.pollRuns();
	}

	loadDesignIntoDraft(d: ResearchDesign): void {
		this.designId = d.id;
		const spec = structuredClone($state.snapshot(d.spec)) as DesignSpec;
		spec.groups.forEach((g, i) => {
			g.color = g.color ?? groupColor(i);
			g.uid = nextUid();
		});
		spec.watch_extra_token_ids ??= [];
		spec.unknown_multi_token_policy ??= 'flag_and_exclude';
		this.draft = spec;
		this.preview = d.compiled ?? null;
		if (!this.preview && spec.groups.length) void this.compilePreview();
	}

	addGroup(name = 'GROUP', role: DesignGroup['role'] = 'target'): DesignGroup {
		const taken = new Set(this.draft.groups.map((g) => g.id));
		const g: DesignGroup = {
			id: slugId(name, taken),
			name,
			role,
			selectors: [],
			color: groupColor(this.draft.groups.length),
			uid: nextUid()
		};
		this.draft.groups.push(g);
		// return the reactive proxy so later mutations are tracked
		return this.draft.groups[this.draft.groups.length - 1];
	}

	/** Rename; while the design is unsaved the id follows the name. */
	renameGroup(g: DesignGroup, name: string): void {
		g.name = name;
		if (this.designId) return;
		const taken = new Set(this.draft.groups.filter((x) => x !== g).map((x) => x.id));
		const old = g.id;
		const id = slugId(name, taken);
		if (id === old) return;
		g.id = id;
		for (const r of this.ruleDraft) if (r.target_group_id === old) r.target_group_id = id;
	}

	/** Add the first token of a multi-token word as an approximate member, then recompile. */
	async useFirstToken(groupId: string, tokenId: number): Promise<void> {
		const g = this.draft.groups.find((x) => x.id === groupId);
		if (!g) return;
		const sel = g.selectors.find((s) => s.kind === 'token_ids');
		if (!sel) g.selectors.push({ kind: 'token_ids', ids: [tokenId] });
		else if (sel.kind === 'token_ids' && !sel.ids.includes(tokenId)) sel.ids.push(tokenId);
		g.approx_ids ??= [];
		if (!g.approx_ids.includes(tokenId)) g.approx_ids.push(tokenId);
		await this.compilePreview();
	}

	/** Fill builders from an untrusted proposal. Only text pieces, never IDs. */
	applyProposal(p: DesignProposal): void {
		this.draft = emptySpec();
		this.designId = null;
		this.preview = null;
		const nameToId = new Map<string, string>();
		const targets = new Set<string>();
		for (const og of p.observation_groups) {
			const g = this.addGroup(og.name, og.role);
			const pieces = cleanPieces(og.candidate_pieces);
			if (pieces.length) g.selectors.push({ kind: 'variant_list', texts: pieces });
			nameToId.set(og.name, g.id);
			if (og.role === 'target') targets.add(og.name);
		}
		// only add_logit_bias on target groups; never exclude
		this.ruleDraft = p.intervention_proposals
			.filter((ip) => nameToId.has(ip.target_group) && targets.has(ip.target_group))
			.filter((ip) => ip.operation === 'add_logit_bias' && Number.isFinite(Number(ip.delta)))
			.map((ip, i) => ({
				id: `r${i + 1}`,
				target_group_id: nameToId.get(ip.target_group)!,
				operation: 'add_logit_bias' as const,
				delta: Number(ip.delta),
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
			// the editor starts over; the saved one is shown in Effective bias
			this.interventionId = iv.id;
			this.ruleDraft = [];
			this.effectiveBias = await ResearchService.effectiveBias(iv.id);
		});
	}

	/** Copy a saved intervention's rules into the editor as a new draft. */
	duplicateIntervention(id: string): void {
		const iv = this.interventions.find((x) => x.id === id);
		if (iv) this.ruleDraft = structuredClone(iv.rules);
	}

	async selectIntervention(id: string): Promise<void> {
		const iv = this.interventions.find((x) => x.id === id);
		if (!iv) return;
		this.interventionId = id;
		await this.run(async () => {
			this.effectiveBias = await ResearchService.effectiveBias(id);
		});
	}

	openRun(id: string): RunView {
		let v = this.views.get(id);
		if (!v) {
			v = new RunView(id, () => void this.syncRuns());
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

/** Dedupe; drop empty, phrase and trailing-whitespace variants; cap at 4. */
export function cleanPieces(pieces: unknown[]): string[] {
	const out: string[] = [];
	for (const p of pieces) {
		if (typeof p !== 'string' || p.trim().length === 0) continue;
		if (/\s$/.test(p)) continue;
		if (/\S\s+\S/.test(p)) continue; // phrases are not single pieces
		if (!out.includes(p)) out.push(p);
	}
	return out.slice(0, 4);
}

export const researchStore = new ResearchStore();
