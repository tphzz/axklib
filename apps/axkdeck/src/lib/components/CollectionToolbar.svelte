<script lang="ts">
    import Icon from './Icon.svelte';

    interface Props {
        title: string;
        count: number;
        query: string;
        onquerychange: (value: string) => void;
        actionLabel?: string;
        actionIcon?: 'upload' | 'broom' | 'sparkles';
        onaction?: () => void;
        filterLabel?: string;
        filterChecked?: boolean;
        onfilterchange?: (checked: boolean) => void;
    }

    let {
        title,
        count,
        query,
        onquerychange,
        actionLabel,
        actionIcon = 'upload',
        onaction = () => undefined,
        filterLabel,
        filterChecked = false,
        onfilterchange = () => undefined,
    }: Props = $props();
</script>

<header class="collection-toolbar">
    <div class="collection-title">
        <h1>{title}</h1>
        <span>{count} {count === 1 ? 'item' : 'items'}</span>
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
