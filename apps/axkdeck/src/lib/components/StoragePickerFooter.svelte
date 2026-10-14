<script lang="ts">
    import type { PickerMode } from '../../features/dialogs/picker';
    let {
        mode,
        selectionAction = 'select',
        hasDirectory,
        hasMore,
        multiple,
        loading,
        opening,
        selectedCount,
        outputName = $bindable(''),
        onloadmore,
        onoutput,
        ondirectory,
        onmedia,
        onfiles,
        oncancel,
    }: {
        mode: PickerMode;
        selectionAction?: 'select' | 'add';
        hasDirectory: boolean;
        hasMore: boolean;
        multiple: boolean;
        loading: boolean;
        opening: boolean;
        selectedCount: number;
        outputName: string;
        onloadmore: () => void;
        onoutput: () => void;
        ondirectory: () => void;
        onmedia: () => void;
        onfiles: () => void;
        oncancel: () => void;
    } = $props();
</script>

<footer class="dialog-footer">
    {#if hasMore}<button class="secondary-button" type="button" disabled={loading} onclick={onloadmore}
            >Load more</button
        >{/if}
    {#if (mode === 'save-file' || mode === 'save-directory') && hasDirectory}
        <input
            class="dialog-field-control"
            bind:value={outputName}
            aria-label={mode === 'save-directory' ? 'Output folder name' : 'Output filename'}
            placeholder={mode === 'save-directory' ? 'Output folder name' : 'Output filename'}
        />
    {/if}
    <div class="dialog-footer-actions">
        {#if selectionAction === 'select'}<button class="secondary-button" type="button" onclick={oncancel}
                >Cancel</button
            >{/if}
        {#if hasDirectory}
            {#if selectionAction === 'add'}
                <button
                    class="secondary-button"
                    type="button"
                    disabled={loading}
                    onclick={selectedCount ? onfiles : ondirectory}
                >
                    {selectedCount
                        ? `Add ${selectedCount} ${selectedCount === 1 ? 'file' : 'files'}`
                        : 'Add current folder'}
                </button>
            {:else if mode === 'save-file' || mode === 'save-directory'}
                <button class="primary-button" type="button" onclick={onoutput}>Select output</button>
            {:else if mode === 'directory'}
                <button class="primary-button" type="button" onclick={ondirectory}>Select directory</button>
            {:else if mode === 'media-source'}
                <button class="primary-button" type="button" disabled={loading || opening} onclick={onmedia}
                    >{opening ? 'Opening' : 'Open current folder'}</button
                >
            {:else if mode === 'floppy-source' && selectedCount === 0}
                <button class="primary-button" type="button" disabled={loading || opening} onclick={ondirectory}
                    >Select current folder</button
                >
            {:else if multiple}
                <button class="primary-button" type="button" disabled={selectedCount === 0} onclick={onfiles}
                    >Select {selectedCount} {selectedCount === 1 ? 'file' : 'files'}</button
                >
            {/if}
        {/if}
    </div>
</footer>
