<script lang="ts">
    import type { Snippet } from 'svelte';

    let {
        name,
        metadata = '',
        nameTitle = name,
        metadataTitle = metadata,
        indicators,
    }: {
        name: string;
        metadata?: string;
        nameTitle?: string;
        metadataTitle?: string;
        indicators?: Snippet;
    } = $props();
</script>

<span class="collection-identity-primary">
    <strong title={nameTitle}>{name}</strong>
    {@render indicators?.()}
</span>
{#if metadata}
    <small class="collection-identity-secondary" title={metadataTitle}>{metadata}</small>
{/if}

<style>
    .collection-identity-primary {
        display: flex;
        min-width: 0;
        align-items: center;
        gap: 3px;
    }

    .collection-identity-primary strong {
        display: block;
        min-width: 0;
        overflow: hidden;
        color: var(--color-text-strong);
        font-size: 10px;
        font-weight: 600;
        line-height: 10px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .collection-identity-secondary {
        display: block;
        margin-top: 0;
        overflow: hidden;
        color: var(--color-text-muted);
        font-size: 8.5px;
        line-height: 9px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
</style>
