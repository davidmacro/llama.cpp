<script lang="ts">
	import type { ChartSeries } from '$lib/utils/research';
	import { fmt } from '$lib/utils/research';

	interface Props {
		onSelect?: (x: number) => void;
		/** Force a shared y domain (compare view). */
		range?: { xMax: number; yMin: number; yMax: number };
		selectedX?: number | null;
		series: ChartSeries[];
		title?: string;
	}

	let { onSelect, range, selectedX = null, series, title = '' }: Props = $props();

	const W = 640;
	const H = 220;
	const PAD = { l: 48, r: 12, t: 10, b: 24 };

	let dom = $derived.by(() => {
		if (range) return range;
		let xMax = 1;
		let yMin = Infinity;
		let yMax = -Infinity;
		for (const s of series)
			for (const p of s.points) {
				if (p.x > xMax) xMax = p.x;
				if (p.y < yMin) yMin = p.y;
				if (p.y > yMax) yMax = p.y;
			}
		if (!Number.isFinite(yMin)) {
			yMin = 0;
			yMax = 1;
		}
		if (yMax - yMin < 1e-9) {
			yMin -= 0.5;
			yMax += 0.5;
		}
		return { xMax, yMin, yMax };
	});

	const sx = (x: number) => PAD.l + (x / Math.max(1, dom.xMax)) * (W - PAD.l - PAD.r);
	const sy = (y: number) => H - PAD.b - ((y - dom.yMin) / (dom.yMax - dom.yMin)) * (H - PAD.t - PAD.b);

	function path(points: { x: number; y: number }[]): string {
		return points.map((p, i) => `${i ? 'L' : 'M'}${sx(p.x).toFixed(1)},${sy(p.y).toFixed(1)}`).join('');
	}

	let ticks = $derived([0, 0.25, 0.5, 0.75, 1].map((f) => dom.yMin + f * (dom.yMax - dom.yMin)));

	function click(e: MouseEvent) {
		if (!onSelect) return;
		const svg = e.currentTarget as SVGSVGElement;
		const r = svg.getBoundingClientRect();
		const px = ((e.clientX - r.left) / r.width) * W;
		const x = Math.round(((px - PAD.l) / (W - PAD.l - PAD.r)) * dom.xMax);
		onSelect(Math.max(0, Math.min(dom.xMax, x)));
	}
</script>

<figure class="space-y-1">
	{#if title}<figcaption class="text-xs text-muted-foreground">{title}</figcaption>{/if}
	<!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_noninteractive_element_interactions -->
	<svg viewBox="0 0 {W} {H}" class="w-full rounded border bg-background" role="img" aria-label="group trajectories" onclick={click}>
		{#each ticks as t, i (i)}
			<line x1={PAD.l} x2={W - PAD.r} y1={sy(t)} y2={sy(t)} stroke="currentColor" stroke-opacity="0.08" />
			<text x={PAD.l - 4} y={sy(t) + 3} font-size="9" text-anchor="end" fill="currentColor" opacity="0.6">{fmt(t, 2)}</text>
		{/each}
		<text x={W - PAD.r} y={H - 6} font-size="9" text-anchor="end" fill="currentColor" opacity="0.6"
			>generation_index (0..{dom.xMax})</text
		>
		{#if selectedX !== null}
			<line x1={sx(selectedX)} x2={sx(selectedX)} y1={PAD.t} y2={H - PAD.b} stroke="currentColor" stroke-opacity="0.4" />
		{/if}
		{#each series as s (s.key)}
			<!-- faint segments only join captured samples; markers are the data -->
			<path d={path(s.points)} fill="none" stroke={s.color} stroke-opacity="0.45" stroke-width="1.5" stroke-dasharray={s.dashed ? '5 4' : undefined} />
			{#each s.points as p, i (i)}
				{#if s.dashed}
					<rect x={sx(p.x) - 2.5} y={sy(p.y) - 2.5} width="5" height="5" fill="none" stroke={s.color} />
				{:else}
					<circle cx={sx(p.x)} cy={sy(p.y)} r="2.5" fill={s.color}><title>{s.label} t={p.x}: {fmt(p.y, 4)}</title></circle>
				{/if}
			{/each}
		{/each}
	</svg>
	<div class="flex flex-wrap gap-3 text-xs">
		{#each series as s (s.key)}
			<span class="flex items-center gap-1">
				<svg width="18" height="6"><line x1="0" x2="18" y1="3" y2="3" stroke={s.color} stroke-width="2" stroke-dasharray={s.dashed ? '4 3' : undefined} /></svg>
				{s.label}{s.points.length === 0 ? ' (no data)' : ''}
			</span>
		{/each}
	</div>
	<p class="text-[10px] text-muted-foreground">
		Markers are captured observations (solid = raw, dashed/square = post_intervention). Connecting
		segments are visual guides only; positions between markers were not sampled.
	</p>
</figure>
