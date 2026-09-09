<script lang="ts">
    import Icon from './Icon.svelte';
    let { mode, onclick }: { mode: 'device' | 'files'; onclick: () => void } = $props();
</script>

<footer class="inspector-mode-footer">
    <button
        class="secondary-button"
        type="button"
        title={mode === 'device' ? 'Show this entry in Device' : 'Reveal this object in Files'}
        {onclick}
    >
        <Icon name={mode === 'device' ? 'hard-drive' : 'folder-open'} size={14} />
        {mode === 'device' ? 'To Device' : 'To Files'}
    </button>
</footer>

<style>
    .inspector-mode-footer {
        flex: none;
        padding: 6px 9px;
        border-top: 1px solid var(--color-border);
        background: var(--color-panel);
    }
    button {
        width: 100%;
        height: 30px;
        margin: 0;
    }
</style>
