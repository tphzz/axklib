<script lang="ts">
    import type { Snippet } from 'svelte';
    import Icon from './Icon.svelte';

    interface Props {
        title: string;
        count: number;
        countText?: string;
        query: string;
        onquerychange: (value: string) => void;
        actionLabel?: string;
        actionIcon?: 'upload' | 'broom' | 'sparkles';
        onaction?: () => void;
        filterLabel?: string;
        filterChecked?: boolean;
        onfilterchange?: (checked: boolean) => void;
        titleControls?: Snippet;
        trailingControls?: Snippet;
    }

    let {
        title,
        count,
        countText,
        query,
        onquerychange,
        actionLabel,
        actionIcon = 'upload',
        onaction = () => undefined,
        filterLabel,
        filterChecked = false,
        onfilterchange = () => undefined,
        titleControls,
        trailingControls,
    }: Props = $props();
</script>

<header class="collection-toolbar">
    <div class="collection-title">
        <h1>{title}</h1>
        <span class="collection-count">{countText ?? `${count} ${count === 1 ? 'item' : 'items'}`}</span>
        {#if titleControls}
            <div class="collection-title-controls">{@render titleControls()}</div>
        {/if}
    </div>
    <div class="collection-actions">
        {#if actionLabel}
            <button class="icon-button" type="button" aria-label={actionLabel} title={actionLabel} onclick={onaction}>
                <Icon name={actionIcon} size={14} />
            </button>
        {/if}
        {#if filterLabel}
            <label class="collection-filter">
                <input
                    type="checkbox"
                    checked={filterChecked}
                    onchange={(event) => onfilterchange(event.currentTarget.checked)}
                />
                <span>{filterLabel}</span>
            </label>
        {/if}
        {#if trailingControls}
            <span class="collection-trailing-controls">{@render trailingControls()}</span>
        {/if}
        <label class="search-field collection-search">
            <Icon name="search" size={14} />
            <input
                value={query}
                oninput={(event) => onquerychange(event.currentTarget.value)}
                type="search"
                placeholder="Search"
                aria-label={`Search ${title}`}
            />
        </label>
    </div>
</header>
