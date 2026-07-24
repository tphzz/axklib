<script lang="ts">
    import { onMount } from 'svelte';
    import type { ImageTransport } from '../transport';
    import { userFacingMessage } from '../userFacingMessage';
    import { modal } from '../modal';
    import {
        serverDirectoryLocation,
        serverFileLocation,
        type DirectoryRef,
        type FileLocation,
        type DirectoryLocation,
        type SandboxEntry,
        type SandboxRoot,
    } from '../storageLocations';
    import Icon from './Icon.svelte';

    type PickerMode = 'file' | 'directory' | 'save-file';
    interface Props {
        transport: ImageTransport;
        mode: PickerMode;
        title: string;
        extensions?: string[];
        suggestedName?: string;
        initialDirectory?: DirectoryRef | null;
        ondirectorychange?: (directory: DirectoryRef | null) => void;
        onmanagelocations?: () => void;
        onselect: (selection: FileLocation | DirectoryLocation) => void;
        oncancel: () => void;
    }

    let {
        transport,
        mode,
        title,
        extensions = [],
        suggestedName = '',
        initialDirectory = null,
        ondirectorychange,
        onmanagelocations,
        onselect,
        oncancel,
    }: Props = $props();

    let roots = $state<SandboxRoot[]>([]);
    let activeRoot = $state<SandboxRoot | null>(null);
    let directory = $state<DirectoryRef | null>(null);
    let entries = $state<SandboxEntry[]>([]);
    let nextCursor = $state<string | null>(null);
    let outputName = $state('');
    let loading = $state(true);
    let error = $state('');
    let creatingDirectory = $state(false);
    let entryActionName = $state('');
    let entryActionBusy = $state(false);
    let entryActionError = $state('');

    const normalizedExtensions = $derived(extensions.map((extension) => extension.toLocaleLowerCase()));
    const visibleEntries = $derived(
        entries.filter((entry) => {
            if (entry.kind === 'DIRECTORY') return true;
            if (mode !== 'file') return false;
            if (normalizedExtensions.length === 0) return true;
            const extension = entry.name.split('.').pop()?.toLocaleLowerCase() ?? '';
            return normalizedExtensions.includes(extension);
        }),
    );

    onMount(async () => {
        outputName = suggestedName;
        try {
            roots = await transport.sandboxRoots();
            if (initialDirectory) {
                const root = roots.find((candidate) => candidate.id === initialDirectory.rootId);
                if (root) {
                    activeRoot = root;
                    if (!(await openDirectory(initialDirectory))) {
                        activeRoot = null;
                        directory = null;
                        entries = [];
                        nextCursor = null;
                        ondirectorychange?.(null);
                    }
                } else {
                    ondirectorychange?.(null);
                }
            }
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            loading = false;
        }
    });

    async function openRoot(root: SandboxRoot): Promise<void> {
        activeRoot = root;
        await openDirectory({ rootId: root.id, relativePath: '' });
    }

    async function openDirectory(reference: DirectoryRef): Promise<boolean> {
        loading = true;
        error = '';
        try {
            const listing = await transport.sandboxDirectory(reference);
            directory = listing.directory;
            entries = listing.entries;
            nextCursor = listing.nextCursor;
            ondirectorychange?.(listing.directory);
            return true;
        } catch (reason) {
            error = userFacingMessage(reason);
            return false;
        } finally {
            loading = false;
        }
    }

    async function loadMore(): Promise<void> {
        if (!directory || !nextCursor || loading) return;
        loading = true;
        try {
            const listing = await transport.sandboxDirectory(directory, nextCursor);
            entries = [...entries, ...listing.entries];
            nextCursor = listing.nextCursor;
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            loading = false;
        }
    }

    function activate(entry: SandboxEntry): void {
        if (!activeRoot) return;
        const reference = { rootId: activeRoot.id, relativePath: entry.relativePath };
        if (entry.kind === 'DIRECTORY') {
            void openDirectory(reference);
            return;
        }
        onselect(serverFileLocation(reference, `${activeRoot.displayName}/${entry.relativePath}`));
    }

    function goUp(): void {
        if (!directory) return;
        if (!directory.relativePath) {
            activeRoot = null;
            directory = null;
            entries = [];
            nextCursor = null;
            ondirectorychange?.(null);
            return;
        }
        const parts = directory.relativePath.split('/');
        parts.pop();
        void openDirectory({ rootId: directory.rootId, relativePath: parts.join('/') });
    }

    function selectCurrentDirectory(): void {
        if (!activeRoot || !directory) return;
        onselect(
            serverDirectoryLocation(
                directory,
                directory.relativePath ? `${activeRoot.displayName}/${directory.relativePath}` : activeRoot.displayName,
            ),
        );
    }

    function selectOutput(): void {
        if (!activeRoot || !directory) return;
        const filename = outputName.trim();
        if (!filename || filename === '.' || filename === '..' || filename.includes('/') || filename.includes('\\')) {
            error = 'Enter a filename without directory separators';
            return;
        }
        if (normalizedExtensions.length > 0) {
            const extension = filename.split('.').pop()?.toLocaleLowerCase() ?? '';
            if (!normalizedExtensions.includes(extension)) {
                error = `Filename must end in ${normalizedExtensions.map((value) => `.${value}`).join(' or ')}`;
                return;
            }
        }
        const relativePath = directory.relativePath ? `${directory.relativePath}/${filename}` : filename;
        onselect(
            serverFileLocation({ rootId: directory.rootId, relativePath }, `${activeRoot.displayName}/${relativePath}`),
        );
    }

    function beginCreateDirectory(): void {
        creatingDirectory = true;
        entryActionName = '';
        entryActionError = '';
    }

    function closeEntryAction(): void {
        if (entryActionBusy) return;
        creatingDirectory = false;
        entryActionError = '';
    }

    function validEntryName(name: string): boolean {
        return Boolean(name) && name !== '.' && name !== '..' && !name.includes('/') && !name.includes('\\');
    }

    async function submitEntryAction(): Promise<void> {
        if (!creatingDirectory || !directory || entryActionBusy) return;
        const name = entryActionName.trim();
        if (!validEntryName(name)) {
            entryActionError = 'Enter a name without directory separators';
            return;
        }

        entryActionBusy = true;
        entryActionError = '';
        try {
            await transport.createSandboxDirectory(directory, name);
            creatingDirectory = false;
            await openDirectory(directory);
        } catch (reason) {
            entryActionError = userFacingMessage(reason);
        } finally {
            entryActionBusy = false;
        }
    }
</script>

<div
    class="dialog-backdrop"
    role="presentation"
    onclick={(event) => {
        if (event.target === event.currentTarget) oncancel();
    }}
>
    <div
        class="dialog-shell dialog-shell-wide storage-picker"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <h2>{title}</h2>
            <button class="icon-button" type="button" aria-label="Close" onclick={oncancel}>×</button>
        </header>

        {#if activeRoot && directory}
            <div class="storage-picker-location">
                <button class="icon-button" type="button" aria-label="Parent directory" onclick={goUp}>←</button>
                <span class="storage-picker-path"
                    >{activeRoot.displayName}{directory.relativePath ? `/${directory.relativePath}` : ''}</span
                >
                {#if activeRoot.writable && mode !== 'file'}
                    <button
                        class="secondary-button storage-picker-directory-action"
                        type="button"
                        onclick={beginCreateDirectory}
                    >
                        <Icon name="folder-plus" size={14} />
                        New folder
                    </button>
                {/if}
            </div>
        {/if}

        <div class="storage-picker-list" aria-busy={loading}>
            {#if !activeRoot}
                {#each roots as root (root.id)}
                    <button
                        class="storage-picker-row"
                        type="button"
                        disabled={(mode === 'directory' || mode === 'save-file') && !root.writable}
                        onclick={() => void openRoot(root)}
                    >
                        <Icon name="folder" size={16} />
                        <span
                            ><strong>{root.displayName}</strong><small
                                >{root.writable ? 'Writable location' : 'Read-only location'}</small
                            ></span
                        >
                        <Icon name="chevron" size={14} />
                    </button>
                {:else}
                    <div class="storage-location-empty">
                        <p>No storage locations are configured.</p>
                        {#if onmanagelocations}
                            <button class="primary-button" type="button" onclick={onmanagelocations}
                                >Manage storage locations</button
                            >
                        {/if}
                    </div>
                {/each}
            {:else}
                {#each visibleEntries as entry (`${entry.kind}:${entry.relativePath}`)}
                    <button
                        class:storage-picker-file-row={entry.kind === 'FILE'}
                        class="storage-picker-row"
                        type="button"
                        onclick={() => activate(entry)}
                    >
                        {#if entry.kind === 'DIRECTORY'}<Icon name="folder" size={16} />{/if}
                        <span
                            ><strong>{entry.name}</strong><small
                                >{entry.kind === 'DIRECTORY' ? 'Directory' : `${entry.size ?? 0} bytes`}</small
                            ></span
                        >
                        {#if entry.kind === 'DIRECTORY'}<Icon name="chevron" size={14} />{/if}
                    </button>
                {/each}
                {#if visibleEntries.length === 0 && !loading}<p class="empty-copy">No matching entries</p>{/if}
            {/if}
            {#if loading}<p class="empty-copy">Loading</p>{/if}
        </div>

        {#if error}<p class="storage-picker-error" role="alert">{error}</p>{/if}

        <footer class="dialog-footer">
            {#if nextCursor}
                <button class="secondary-button" type="button" disabled={loading} onclick={() => void loadMore()}
                    >Load more</button
                >
            {/if}
            {#if mode === 'save-file' && directory}
                <input bind:value={outputName} aria-label="Output filename" placeholder="Output filename" />
                <button class="primary-button" type="button" onclick={selectOutput}>Select output</button>
            {:else if mode === 'directory' && directory}
                <button class="primary-button" type="button" onclick={selectCurrentDirectory}>Select directory</button>
            {/if}
            <button class="secondary-button" type="button" onclick={oncancel}>Cancel</button>
        </footer>
    </div>
</div>

{#if creatingDirectory}
    <div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
        <div
            class="dialog-shell entry-action-dialog"
            role="dialog"
            aria-modal="true"
            aria-label="Create folder"
            use:modal={{ onescape: closeEntryAction }}
        >
            <header class="dialog-header">
                <h2>New folder</h2>
                <button class="icon-button" type="button" aria-label="Close" onclick={closeEntryAction}>×</button>
            </header>
            <div class="entry-action-content">
                <label>
                    <span>Folder name</span>
                    <input aria-label="Folder name" bind:value={entryActionName} disabled={entryActionBusy} />
                </label>
                {#if entryActionError}<p class="storage-picker-error" role="alert">{entryActionError}</p>{/if}
            </div>
            <footer class="dialog-footer">
                <button class="secondary-button" type="button" disabled={entryActionBusy} onclick={closeEntryAction}
                    >Cancel</button
                >
                <button
                    class="primary-button"
                    type="button"
                    disabled={entryActionBusy}
                    onclick={() => void submitEntryAction()}
                >
                    Create
                </button>
            </footer>
        </div>
    </div>
{/if}
