<script lang="ts">
	import { b64ToHex, isNonUtf8, visiblePiece } from '$lib/utils/research';

	interface Props {
		b64?: string;
		class?: string;
		piece: string;
	}

	let { b64, class: className = '', piece }: Props = $props();

	let nonUtf8 = $derived(isNonUtf8(b64));
</script>

<!-- token text is interpolated as text only, never as HTML -->
{#if nonUtf8}
	<code class="rounded bg-amber-500/15 px-1 font-mono text-xs {className}" title="non-UTF8 bytes: {b64ToHex(b64)}"
		>b64:{b64}</code
	>
{:else}
	<code class="rounded bg-muted px-1 font-mono text-xs whitespace-pre {className}" title={JSON.stringify(piece)}
		>{visiblePiece(piece)}</code
	>
{/if}
