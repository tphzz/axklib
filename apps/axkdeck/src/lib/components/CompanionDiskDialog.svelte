<script lang="ts">
    import type { DirectoryRef } from '../storageLocations';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        directories: DirectoryRef[];
        busy: boolean;
        error: string;
        onadd: () => void;
        onremove: (directory: DirectoryRef) => void;
        onnearby: () => void;
        onconfirm: () => void;
        oncancel: () => void;
    }

    let { directories, busy, error, onadd, onremove, onnearby, onconfirm, oncancel }: Props = $props();
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell companion-disk-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Add companion disks"
        aria-busy={busy}
        use:modal={{ onescape: busy ? undefined : oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="folder-plus" size={16} />
                <h2>Add companion disks</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={oncancel}>×</button>
        </header>

        <div class="companion-disk-content">
            <p>Wave Data continues on another sampler disk. Add the folders that belong to this disk set.</p>
            <div class="companion-disk-actions">
                <button class="secondary-button" type="button" disabled={busy} onclick={onadd}>
                    <Icon name="folder-plus" size={15} />
                    Add folder
                </button>
                <button class="secondary-button" type="button" disabled={busy} onclick={onnearby}>
                    <Icon name="search" size={15} />
                    Search nearby folders and retry
                </button>
            </div>

            <section aria-label="Companion disk folders">
                <h3>Selected folders</h3>
                {#if directories.length === 0}
                    <p class="companion-disk-empty">No companion folders selected</p>
                {:else}
                    <div class="companion-disk-list">
                        {#each directories as directory (`${directory.rootId}:${directory.relativePath}`)}
                            <div class="companion-disk-row">
                                <Icon name="folder" size={15} />
                                <span>
                                    <strong>{directory.relativePath || directory.rootId}</strong>
                                    {#if directory.relativePath}<small>{directory.rootId}</small>{/if}
                                </span>
                                <button
                                    class="icon-button"
                                    type="button"
                                    aria-label={`Remove ${directory.relativePath || directory.rootId}`}
                                    disabled={busy}
                                    onclick={() => onremove(directory)}
                                >
                                    <Icon name="close" size={14} />
                                </button>
                            </div>
                        {/each}
                    </div>
                {/if}
            </section>
            {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" disabled={busy} onclick={oncancel}>Cancel</button>
            <button
                class="primary-button"
                type="button"
                disabled={busy || directories.length === 0}
                onclick={onconfirm}
            >
                {busy ? 'Adding folders' : 'Add and retry'}
            </button>
        </footer>
    </div>
</div>

<style>
    .companion-disk-dialog {
        width: min(560px, calc(100vw - 32px));
    }

    .companion-disk-content {
        display: grid;
        gap: 14px;
        padding: 16px;
    }

    .companion-disk-content > p,
    .companion-disk-empty {
        margin: 0;
        color: var(--color-text-muted);
    }

    .companion-disk-content h3 {
        margin: 0 0 8px;
        font-size: 12px;
        font-weight: 600;
    }

    .companion-disk-actions {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
    }

    .companion-disk-actions button {
        display: inline-flex;
        align-items: center;
        gap: 7px;
    }

    .companion-disk-list {
        border: 1px solid var(--color-border);
        max-height: 220px;
        overflow: auto;
    }

    .companion-disk-row {
        display: grid;
        grid-template-columns: auto minmax(0, 1fr) auto;
        align-items: center;
        gap: 9px;
        min-height: 42px;
        padding: 5px 7px 5px 10px;
    }

    .companion-disk-row + .companion-disk-row {
        border-top: 1px solid var(--color-border);
    }

    .companion-disk-row span {
        display: grid;
        min-width: 0;
    }

    .companion-disk-row strong,
    .companion-disk-row small {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }

    .companion-disk-row small {
        color: var(--color-text-muted);
    }
</style>
