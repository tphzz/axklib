<script lang="ts">
    import { onMount } from 'svelte';
    import type { ImageTransport } from '../transport';
    import { userFacingMessage } from '../userFacingMessage';
    import {
        serverDirectoryLocation,
        serverFileLocation,
        type DirectoryRef,
        type FileRef,
        type FileLocation,
        type DirectoryLocation,
        type SandboxEntry,
        type SandboxRoot,
    } from '../storageLocations';
    import Icon from './Icon.svelte';

    type PickerMode = 'file' | 'directory' | 'save-file';
    type EntryAction =
        { kind: 'create' } | { kind: 'rename'; entry: SandboxEntry } | { kind: 'delete'; entry: SandboxEntry };
    interface DirectoryAction {
        label: string;
        icon: 'hard-drive' | 'folder-plus';
        onactivate: (directory: DirectoryLocation) => void;
    }

    interface Props {
        transport: ImageTransport;
        mode: PickerMode;
        title: string;
        extensions?: string[];
        suggestedName?: string;
        initialDirectory?: DirectoryRef | null;
        ondirectorychange?: (directory: DirectoryRef | null) => void;
        directoryAction?: DirectoryAction;
        onbeforedelete?: (entry: FileRef) => Promise<void>;
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
        directoryAction,
        onbeforedelete = async () => undefined,
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
    let menuEntryPath = $state<string | null>(null);
    let menuPosition = $state({ top: 0, left: 0 });
    let entryAction = $state<EntryAction | null>(null);
    let entryActionName = $state('');
    let entryActionBusy = $state(false);
    let entryActionError = $state('');

    const normalizedExtensions = $derived(extensions.map((extension) => extension.toLocaleLowerCase()));
    const visibleEntries = $derived(
        entries.filter((entry) => {
            if (entry.kind === 'directory') return true;
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
        menuEntryPath = null;
        const reference = { rootId: activeRoot.id, relativePath: entry.relativePath };
        if (entry.kind === 'directory') {
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

    function activateDirectoryAction(): void {
        if (!directoryAction || !activeRoot || !directory) return;
        directoryAction.onactivate(
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
        entryAction = { kind: 'create' };
        entryActionName = '';
        entryActionError = '';
        menuEntryPath = null;
    }

    function beginRename(entry: SandboxEntry): void {
        entryAction = { kind: 'rename', entry };
        entryActionName = entry.name;
        entryActionError = '';
        menuEntryPath = null;
    }

    function beginDelete(entry: SandboxEntry): void {
        entryAction = { kind: 'delete', entry };
        entryActionName = '';
        entryActionError = '';
        menuEntryPath = null;
    }

    function toggleEntryMenu(entry: SandboxEntry, event: MouseEvent): void {
        if (menuEntryPath === entry.relativePath) {
            menuEntryPath = null;
            return;
        }

        const trigger = event.currentTarget;
        if (!(trigger instanceof HTMLElement)) return;

        const triggerBounds = trigger.getBoundingClientRect();
        const dialogBounds = trigger.closest('.storage-picker')?.getBoundingClientRect();
        const menuWidth = 118;
        const menuHeight = 64;
        const gap = 2;
        const margin = 8;
        const boundaryTop = Math.max(margin, dialogBounds?.top ?? margin);
        const boundaryRight = Math.min(window.innerWidth - margin, (dialogBounds?.right ?? window.innerWidth) - margin);
        const boundaryBottom = Math.min(
            window.innerHeight - margin,
            (dialogBounds?.bottom ?? window.innerHeight) - margin,
        );
        const top =
            triggerBounds.bottom + gap + menuHeight <= boundaryBottom
                ? triggerBounds.bottom + gap
                : Math.max(boundaryTop, triggerBounds.top - gap - menuHeight);
        const left = Math.max(margin, Math.min(triggerBounds.right - menuWidth, boundaryRight - menuWidth));

        menuPosition = { top, left };
        menuEntryPath = entry.relativePath;
    }

    function closeEntryAction(): void {
        if (entryActionBusy) return;
        entryAction = null;
        entryActionError = '';
    }

    function validEntryName(name: string): boolean {
        return Boolean(name) && name !== '.' && name !== '..' && !name.includes('/') && !name.includes('\\');
    }

    async function submitEntryAction(): Promise<void> {
        if (!entryAction || !directory || entryActionBusy) return;
        const action = entryAction;
        const name = entryActionName.trim();
        if (action.kind !== 'delete' && !validEntryName(name)) {
            entryActionError = 'Enter a name without directory separators';
            return;
        }

        entryActionBusy = true;
        entryActionError = '';
        try {
            if (action.kind === 'create') {
                await transport.createSandboxDirectory(directory, name);
            } else {
                const reference = { rootId: directory.rootId, relativePath: action.entry.relativePath };
                if (action.kind === 'rename') {
                    await transport.renameSandboxEntry(reference, name);
                } else {
                    await onbeforedelete(reference);
                    await transport.deleteSandboxEntry(reference);
                }
            }
            entryAction = null;
            await openDirectory(directory);
        } catch (reason) {
            entryActionError = userFacingMessage(reason);
        } finally {
            entryActionBusy = false;
        }
    }
</script>

<svelte:window onresize={() => (menuEntryPath = null)} />

<div
    class="dialog-backdrop"
    role="presentation"
    onclick={(event) => {
        if (event.target === event.currentTarget) oncancel();
    }}
>
    <div class="dialog-shell dialog-shell-wide storage-picker" role="dialog" aria-modal="true" aria-label={title}>
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
                {#if activeRoot.writable}
                    {#if directoryAction}
                        <button
                            class="secondary-button storage-picker-directory-action"
                            type="button"
                            onclick={activateDirectoryAction}
                        >
                            <Icon name={directoryAction.icon} size={14} />
                            {directoryAction.label}
                        </button>
                    {/if}
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

        <div class="storage-picker-list" aria-busy={loading} onscroll={() => (menuEntryPath = null)}>
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
                                >{root.writable ? 'Writable root' : 'Read-only root'}</small
                            ></span
                        >
                        <Icon name="chevron" size={14} />
                    </button>
                {/each}
            {:else}
                {#each visibleEntries as entry (`${entry.kind}:${entry.relativePath}`)}
                    <div class="storage-picker-entry">
                        <button
                            class:storage-picker-file-row={entry.kind === 'file'}
                            class="storage-picker-row"
                            type="button"
                            onclick={() => activate(entry)}
                        >
                            {#if entry.kind === 'directory'}<Icon name="folder" size={16} />{/if}
                            <span
                                ><strong>{entry.name}</strong><small
                                    >{entry.kind === 'directory' ? 'Directory' : `${entry.size ?? 0} bytes`}</small
                                ></span
                            >
                            {#if entry.kind === 'directory'}<Icon name="chevron" size={14} />{/if}
                        </button>
                        {#if activeRoot.writable}
                            <div class="storage-picker-entry-actions">
                                <button
                                    class="icon-button"
                                    type="button"
                                    aria-label={`More actions for ${entry.name}`}
                                    aria-expanded={menuEntryPath === entry.relativePath}
                                    onclick={(event) => toggleEntryMenu(entry, event)}
                                >
                                    <Icon name="more" size={15} />
                                </button>
                            </div>
                        {/if}
                    </div>
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

    {#if menuEntryPath}
        {@const menuEntry = entries.find((entry) => entry.relativePath === menuEntryPath)}
        {#if menuEntry}
            <div
                class="storage-picker-entry-menu"
                role="menu"
                style={`top: ${menuPosition.top}px; left: ${menuPosition.left}px;`}
            >
                <button type="button" role="menuitem" onclick={() => beginRename(menuEntry)}>
                    <Icon name="rename" size={14} /> Rename
                </button>
                <button type="button" role="menuitem" onclick={() => beginDelete(menuEntry)}>
                    <Icon name="trash" size={14} /> Delete
                </button>
            </div>
        {/if}
    {/if}
</div>

{#if entryAction}
    <div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
        <div
            class="dialog-shell entry-action-dialog"
            role="dialog"
            aria-modal="true"
            aria-label={entryAction.kind === 'create'
                ? 'Create folder'
                : entryAction.kind === 'rename'
                  ? `Rename ${entryAction.entry.name}`
                  : `Delete ${entryAction.entry.name}`}
        >
            <header class="dialog-header">
                <h2>
                    {entryAction.kind === 'create'
                        ? 'New folder'
                        : entryAction.kind === 'rename'
                          ? 'Rename entry'
                          : 'Delete entry'}
                </h2>
                <button class="icon-button" type="button" aria-label="Close" onclick={closeEntryAction}>×</button>
            </header>
            <div class="entry-action-content">
                {#if entryAction.kind === 'delete'}
                    <strong>Delete “{entryAction.entry.name}”?</strong>
                    <p>This action cannot be undone.</p>
                {:else}
                    <label>
                        <span>{entryAction.kind === 'create' ? 'Folder name' : 'Entry name'}</span>
                        <input
                            aria-label={entryAction.kind === 'create' ? 'Folder name' : 'Entry name'}
                            bind:value={entryActionName}
                            disabled={entryActionBusy}
                        />
                    </label>
                {/if}
                {#if entryActionError}<p class="storage-picker-error" role="alert">{entryActionError}</p>{/if}
            </div>
            <footer class="dialog-footer">
                <button class="secondary-button" type="button" disabled={entryActionBusy} onclick={closeEntryAction}
                    >Cancel</button
                >
                <button
                    class={entryAction.kind === 'delete' ? 'danger-button' : 'primary-button'}
                    type="button"
                    disabled={entryActionBusy}
                    onclick={() => void submitEntryAction()}
                >
                    {entryAction.kind === 'create'
                        ? 'Create'
                        : entryAction.kind === 'rename'
                          ? 'Rename entry'
                          : 'Delete permanently'}
                </button>
            </footer>
        </div>
    </div>
{/if}
