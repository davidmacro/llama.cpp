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

/** Make spaces, tabs, newlines and other control chars visible. */
export function visiblePiece(piece: string): string {
	let out = '';
	for (const ch of piece) {
		const c = ch.codePointAt(0) ?? 0;
		if (ch === ' ') out += '␣';
		else if (ch === '\n') out += '↵';
		else if (ch === '\t') out += '⇥';
		else if (ch === '\r') out += '\\r';
		else if (c < 0x20 || c === 0x7f) out += `\\x${c.toString(16).padStart(2, '0')}`;
		else if (ch === '�') out += '�';
		else out += ch;
	}
	return out;
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
