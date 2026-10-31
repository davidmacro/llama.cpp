// Helpers for the research workspace. Token text is always rendered as text
// (never HTML); these helpers only make whitespace/control bytes visible.

export const GROUP_COLORS = [
	'#2563eb',
	'#dc2626',
	'#16a34a',
	'#d97706',
	'#9333ea',
	'#0891b2',
	'#db2777',
	'#65a30d'
];

export function groupColor(index: number): string {
	return GROUP_COLORS[index % GROUP_COLORS.length];
}

/** Whitespace glyphs (muted in ResearchPiece); ASCII source via escapes. */
export const WS_GLYPH: Record<string, string> = {
	' ': '\u00b7',
	'\n': '\u21b5',
	'\t': '\u21e5',
	'\r': '\r'
};

export const WS_TITLE: Record<string, string> = {
	' ': 'space',
	'\n': 'newline',
	'\t': 'tab',
	'\r': 'carriage return'
};

export interface PieceSegment {
	text: string;
	ws: boolean;
	title?: string;
}

/** Split a piece into text and visible whitespace/control segments. */
export function pieceSegments(piece: string): PieceSegment[] {
	const out: PieceSegment[] = [];
	for (const ch of piece) {
		const c = ch.codePointAt(0) ?? 0;
		let seg: PieceSegment;
		if (WS_GLYPH[ch]) seg = { text: WS_GLYPH[ch], ws: true, title: WS_TITLE[ch] };
		else if (c < 0x20 || c === 0x7f)
			seg = { text: `\\x${c.toString(16).padStart(2, '0')}`, ws: true, title: 'control byte' };
		else seg = { text: ch, ws: false };
		const last = out[out.length - 1];
		if (last && !last.ws && !seg.ws) last.text += seg.text;
		else out.push(seg);
	}
	return out;
}

/** Plain-string version of pieceSegments (titles, option labels). */
export function visiblePiece(piece: string): string {
	return pieceSegments(piece)
		.map((s) => s.text)
		.join('');
}

/** True when the base64 bytes are not valid UTF-8 (partial byte tokens). */
export function isNonUtf8(b64?: string): boolean {
	if (!b64) return false;
	try {
		const bin = atob(b64);
		const bytes = Uint8Array.from(bin, (c) => c.charCodeAt(0));
		new TextDecoder('utf-8', { fatal: true }).decode(bytes);
		return false;
	} catch {
		return true;
	}
}

export function b64ToHex(b64?: string): string {
	if (!b64) return '';
	try {
		return Array.from(atob(b64), (c) => c.charCodeAt(0).toString(16).padStart(2, '0')).join(' ');
	} catch {
		return '';
	}
}

export function fmt(v: number | null | undefined, digits = 3): string {
	if (v === null || v === undefined || Number.isNaN(v)) return '-';
	if (v === -Infinity) return '-inf';
	if (v === Infinity) return '+inf';
	return v.toFixed(digits);
}

/** Mass with adaptive precision: percent, or scientific below 0.001. */
export function fmtMass(p: number | null | undefined): string {
	if (p === null || p === undefined || !Number.isFinite(p)) return '-';
	if (p === 0) return '0';
	if (p < 0.001) return p.toExponential(1);
	const pct = p * 100;
	return `${pct.toFixed(pct >= 10 ? 1 : 2)}%`;
}

/** Native select styling that survives @tailwindcss/forms padding. */
export const SELECT_CLASS =
	'h-8 rounded-md border border-input bg-transparent py-0 pr-8 pl-2 text-sm leading-normal dark:bg-input/30';

/** p' = p e^d / (1 - p + p e^d): mass of a set after adding d to all its logits. */
export function shiftedMass(p: number, d: number): number {
	if (d === -Infinity) return 0;
	const e = Math.exp(d);
	return (p * e) / (1 - p + p * e);
}

export function slugId(name: string, taken: Set<string>): string {
	const baseId =
		name
			.toLowerCase()
			.replace(/[^a-z0-9]+/g, '_')
			.replace(/^_+|_+$/g, '') || 'group';
	let id = baseId;
	let i = 2;
	while (taken.has(id)) id = `${baseId}_${i++}`;
	return id;
}

export function shortId(id: string | null | undefined): string {
	if (!id) return '-';
	const i = id.indexOf('_');
	return i >= 0 && id.length > i + 7 ? `${id.slice(0, i + 7)}` : id;
}

import type { GroupMetric, MetricKey, Observation } from '$lib/types/research';

export interface ChartSeries {
	key: string;
	label: string;
	color: string;
	dashed: boolean;
	points: { x: number; y: number }[];
}

export const METRIC_LABELS: Record<MetricKey, string> = {
	lse: 'logsumexp of member logits',
	rel: 'watch-relative mass',
	abs: 'absolute mass (watch_normalized only)'
};

/** Raw (solid) and post_intervention (dashed) series per group; only captured positions. */
export function buildSeries(
	observations: Observation[],
	groups: { id: string; name: string }[],
	metric: MetricKey,
	colorFor: (groupId: string, index: number) => string,
	labelPrefix = ''
): ChartSeries[] {
	const out: ChartSeries[] = [];
	groups.forEach((g, gi) => {
		const raw: { x: number; y: number }[] = [];
		const adj: { x: number; y: number }[] = [];
		for (const o of observations) {
			const m: GroupMetric | undefined = o.groups.find((x) => x.id === g.id) ?? o.groups[gi];
			if (!m) continue;
			const r = m[`raw_${metric}` as keyof GroupMetric] as number | null;
			const a = m[`adj_${metric}` as keyof GroupMetric] as number | null;
			if (typeof r === 'number' && Number.isFinite(r)) raw.push({ x: o.generation_index, y: r });
			if (typeof a === 'number' && Number.isFinite(a)) adj.push({ x: o.generation_index, y: a });
		}
		const color = colorFor(g.id, gi);
		out.push({ key: `${labelPrefix}${g.id}:raw`, label: `${labelPrefix}${g.name} raw`, color, dashed: false, points: raw });
		out.push({ key: `${labelPrefix}${g.id}:adj`, label: `${labelPrefix}${g.name} post_intervention`, color, dashed: true, points: adj });
	});
	return out;
}

/** File name of a model path (full path stays in tooltips). */
export function baseName(path: string | undefined): string {
	if (!path) return '-';
	const parts = path.split(/[\/]/);
	return parts[parts.length - 1] || path;
}

/** Signed delta label, e.g. "-4" or "+2". */
export function signed(d: number): string {
	return d > 0 ? `+${d}` : String(d);
}

/** Human summary of an intervention's rules, e.g. "NEGATIVE -4". */
export function ruleSummary(
	rules: { target_group_id: string; operation: string; delta: number; enabled: boolean }[],
	nameOf: (groupId: string) => string
): string {
	const on = rules.filter((r) => r.enabled);
	if (on.length === 0) return 'no active rules';
	return on
		.map((r) => `${nameOf(r.target_group_id)} ${r.operation === 'exclude' ? 'EXCLUDE' : signed(r.delta)}`)
		.join(', ');
}

/** ln(p / (1 - p)); null outside (0, 1). */
export function logOdds(p: number | null | undefined): number | null {
	if (p === null || p === undefined || !(p > 0 && p < 1)) return null;
	return Math.log(p / (1 - p));
}

/** Drop end-of-generation / special markers such as <|im_end|> or </s> for display. */
export function stripEog(text: string | null | undefined): string {
	return (text ?? '')
		.replace(/<\|[^|<>]{1,40}\|>/g, '')
		.replace(/<\/s>|<eos>|<end_of_turn>/g, '')
		.trim();
}

/** Top group (by post_intervention lse) and its mass at one observation. */
export function topGroup(
	o: Observation | undefined,
	stage: 'raw' | 'adj' = 'adj'
): { id: string; mass: number | null; kind: 'abs' | 'rel' } | null {
	if (!o) return null;
	const kind: 'abs' | 'rel' = o.groups.some((g) => g[`${stage}_abs`] !== null) ? 'abs' : 'rel';
	let best: GroupMetric | null = null;
	for (const g of o.groups) {
		const v = g[`${stage}_lse`];
		if (typeof v !== 'number' || !Number.isFinite(v)) continue;
		if (!best || v > (best[`${stage}_lse`] as number)) best = g;
	}
	return best ? { id: best.id, mass: best[`${stage}_${kind}`], kind } : null;
}

/** Earliest time stamp of a run (for batch ordering). */
export function runTime(r: { created_at?: string; started_at?: string }): string {
	return r.created_at ?? r.started_at ?? '';
}
