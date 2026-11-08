<script lang="ts">
    import Icon from './Icon.svelte';
    let {
        label,
        count,
        page,
        pageSize = 100,
        onchange,
    }: {
        label: string;
        count: number;
        page: number;
        pageSize?: number;
        onchange: (page: number) => void;
    } = $props();
    const pages = $derived(Math.max(1, Math.ceil(count / pageSize)));
</script>

<nav aria-label={`${label} pages`}>
    <span>{count ? page * pageSize + 1 : 0}-{Math.min(count, (page + 1) * pageSize)} of {count} {label}</span>
    <button
        class="icon-button previous"
        aria-label={`Previous ${label} page`}
        title={`Previous ${label} page`}
        disabled={page === 0}
        onclick={() => onchange(page - 1)}><Icon name="chevron" size={14} /></button
    >
    <button
        class="icon-button"
        aria-label={`Next ${label} page`}
        title={`Next ${label} page`}
        disabled={page + 1 >= pages}
        onclick={() => onchange(page + 1)}><Icon name="chevron" size={14} /></button
    >
</nav>

<style>
    nav {
        display: flex;
        align-items: center;
        gap: 4px;
        min-height: 28px;
    }
    span {
        flex: 1;
        font-size: var(--dialog-metadata-font-size);
        color: var(--color-text-muted);
    }
    .previous :global(svg) {
        transform: rotate(180deg);
    }
</style>
