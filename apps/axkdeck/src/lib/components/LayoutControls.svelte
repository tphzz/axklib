<script lang="ts">
    import Icon from './Icon.svelte';

    interface Props {
        libraryOpen: boolean;
        editorOpen: boolean;
        editorAvailable: boolean;
        inspectorOpen: boolean;
        ontogglelibrary: () => void;
        ontoggleeditor: () => void;
        ontoggleinspector: () => void;
    }

    let {
        libraryOpen,
        editorOpen,
        editorAvailable,
        inspectorOpen,
        ontogglelibrary,
        ontoggleeditor,
        ontoggleinspector,
    }: Props = $props();
</script>

<div class="layout-controls" role="toolbar" aria-label="Panel layout">
    <button
        class:active={libraryOpen}
        class="icon-button"
        type="button"
        aria-label="Library panel"
        aria-pressed={libraryOpen}
        title="Library panel"
        onclick={ontogglelibrary}
    >
        <Icon name="panel-left" size={17} />
    </button>
    <button
        class:active={editorOpen}
        class="icon-button"
        type="button"
        aria-label="Editor panel"
        aria-pressed={editorOpen}
        aria-disabled={!editorAvailable}
        disabled={!editorAvailable}
        title="Editor panel"
        onclick={ontoggleeditor}
    >
        <Icon name="panel-bottom" size={17} />
    </button>
    <button
        class:active={inspectorOpen}
        class="icon-button inspector-toggle"
        type="button"
        aria-label="Inspector panel"
        aria-pressed={inspectorOpen}
        title="Inspector panel"
        onclick={ontoggleinspector}
    >
        <Icon name="panel-right" size={17} />
    </button>
</div>

<style>
    .layout-controls {
        display: flex;
        flex: none;
        align-items: center;
        gap: 2px;
        padding-left: 10px;
        border-left: 1px solid var(--color-border);
    }

    @media (max-width: 850px) {
        .inspector-toggle {
            display: none;
        }
    }
</style>
