<script lang="ts">
    import Icon from './Icon.svelte';

    interface Props {
        count: number;
        onexport?: () => void;
        ondelete?: () => void;
        onclear: () => void;
    }

    let { count, onexport, ondelete, onclear }: Props = $props();
</script>

<div class="package-selection-controls" aria-label="Object selection">
    <span role="status">{count} selected</span>
    {#if onexport}
        <button
            class="package-selection-action package-selection-export"
            type="button"
            aria-label={`Export ${count} selected ${count === 1 ? 'object' : 'objects'}`}
            title="Export selected objects"
            onclick={onexport}
        >
            <Icon name="archive" size={14} /><span>Export</span>
        </button>
    {/if}
    {#if ondelete}
        <button
            class="package-selection-action package-selection-delete"
            type="button"
            aria-label={`Delete ${count} selected ${count === 1 ? 'object' : 'objects'}`}
            title="Delete selected objects"
            onclick={ondelete}
        >
            <Icon name="trash" size={14} /><span>Delete</span>
        </button>
    {/if}
    <button
        class="icon-button"
        type="button"
        aria-label="Clear object selection"
        title="Clear object selection"
        onclick={onclear}
    >
        <Icon name="close" size={13} />
    </button>
</div>
