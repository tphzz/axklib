<script lang="ts">
    import Icon from './Icon.svelte';

    interface Props {
        count: number;
        onexport: () => void;
        onclear: () => void;
    }

    let { count, onexport, onclear }: Props = $props();
</script>

<div class="package-selection-controls" aria-label="Package export selection">
    <span role="status">{count} selected</span>
    <button
        class="package-selection-export"
        type="button"
        aria-label={`Export ${count} selected ${count === 1 ? 'object' : 'objects'}`}
        title="Export selected objects"
        onclick={onexport}
    >
        <Icon name="archive" size={14} /><span>Export</span>
    </button>
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
