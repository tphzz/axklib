<script lang="ts">
    import type { Snippet } from 'svelte';
    import Icon from './Icon.svelte';

    let {
        label,
        heading,
        query = $bindable(''),
        searchLabel,
        searchable = true,
        navigation = false,
        children,
    }: {
        label: string;
        heading: string;
        query?: string;
        searchLabel: string;
        searchable?: boolean;
        navigation?: boolean;
        children: Snippet;
    } = $props();
</script>

<section class="image-contents" aria-label={label} role={navigation ? 'navigation' : undefined}>
    <div class="panel-heading">
        <div>
            <p class="eyebrow">Contents</p>
            <h2>{heading}</h2>
        </div>
    </div>
    {#if searchable}
        <label class="search-field mx-3 mb-2">
            <Icon name="search" size={15} />
            <input bind:value={query} type="search" placeholder="Search" aria-label={searchLabel} />
        </label>
    {/if}
    {@render children()}
</section>
