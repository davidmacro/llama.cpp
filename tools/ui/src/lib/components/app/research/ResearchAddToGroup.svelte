<script lang="ts">
	import * as DropdownMenu from '$lib/components/ui/dropdown-menu';
	import { researchStore as rs } from '$lib/stores/research.svelte';
	import { groupColor } from '$lib/utils/research';

	interface Props {
		tokenId: number;
	}

	let { tokenId }: Props = $props();

	const inGroup = (ids: { kind: string; ids?: number[] }[]) => ids.some((s) => s.kind === 'token_ids' && s.ids?.includes(tokenId));
</script>

<DropdownMenu.Root>
	<DropdownMenu.Trigger>
		{#snippet child({ props })}
			<button
				{...props}
				class="rounded px-1 text-[10px] leading-4 text-muted-foreground hover:bg-muted hover:text-foreground data-[state=open]:bg-muted"
				title="Add token {tokenId} to a hypertoken group"
				aria-label="Add token {tokenId} to a group">+grp</button
			>
		{/snippet}
	</DropdownMenu.Trigger>
	<DropdownMenu.Content align="start" class="min-w-44">
		<DropdownMenu.Label class="text-xs">Add #{tokenId} to group (draft)</DropdownMenu.Label>
		{#each rs.draft.groups as g, i (g.uid ?? g.id)}
			<DropdownMenu.Item class="gap-2 text-sm" disabled={inGroup(g.selectors)} onSelect={() => rs.addTokenToGroup(g.id, tokenId)}>
				<span class="h-2.5 w-2.5 shrink-0 rounded-full" style:background={g.color ?? groupColor(i)}></span>
				<span class="flex-1 truncate">{g.name}</span>
				{#if inGroup(g.selectors)}<span class="text-[10px] text-muted-foreground">already</span>{/if}
			</DropdownMenu.Item>
		{:else}
			<DropdownMenu.Item disabled class="text-xs">No groups in the draft design</DropdownMenu.Item>
		{/each}
	</DropdownMenu.Content>
</DropdownMenu.Root>
