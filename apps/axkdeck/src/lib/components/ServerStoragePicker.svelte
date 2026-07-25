<script lang="ts">
    import { onMount, tick } from 'svelte';
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
    let listElement = $state<HTMLDivElement | null>(null);
    let activeOptionIndex = $state(-1);

    const normalizedExtensions = $derived(extensions.map((extension) => extension.toLocaleLowerCase()));
    const visibleEntries = $derived(entries.filter(entryIsVisible));
    const activeOptionId = $derived(
        activeOptionIndex >= 0
            ? `storage-picker-option-${activeRoot ? 'entry' : 'root'}-${activeOptionIndex}`
            : undefined,
    );

    onMount(async () => {
        queueMicrotask(() => listElement?.focus());
        outputName = suggestedName;
        try {
            roots = await transport.sandboxRoots();
            selectFirstOption();
            if (initialDirectory) {
                const root = roots.find((candidate) => candidate.id === initialDirectory.rootId);
                if (root) {
                    if (!(await openDirectory(initialDirectory, root))) ondirectorychange?.(null);
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

    function entryIsVisible(entry: SandboxEntry): boolean {
        if (entry.kind === 'DIRECTORY') return true;
        if (mode !== 'file') return false;
        if (normalizedExtensions.length === 0) return true;
        const extension = entry.name.split('.').pop()?.toLocaleLowerCase() ?? '';
        return normalizedExtensions.includes(extension);
    }

    function rootIsDisabled(root: SandboxRoot): boolean {
        return (mode === 'directory' || mode === 'save-file') && !root.writable;
    }

    function enabledOptionIndices(): number[] {
        if (activeRoot) return visibleEntries.map((_, index) => index);
        return roots.flatMap((root, index) => (rootIsDisabled(root) ? [] : [index]));
    }

    function selectFirstOption(): void {
        activeOptionIndex = enabledOptionIndices()[0] ?? -1;
    }

    async function revealActiveOption(): Promise<void> {
        await tick();
        const option = listElement?.querySelector<HTMLElement>(`[data-picker-index="${activeOptionIndex}"]`);
        option?.scrollIntoView?.({ block: 'nearest' });
    }

    function setActiveOption(index: number): void {
        if (!enabledOptionIndices().includes(index)) return;
        activeOptionIndex = index;
        void revealActiveOption();
    }

    function moveActiveOption(direction: -1 | 1): void {
        const enabled = enabledOptionIndices();
        if (enabled.length === 0) return;
        const position = enabled.indexOf(activeOptionIndex);
        const current = position >= 0 ? position : 0;
        const next = Math.max(0, Math.min(enabled.length - 1, current + direction));
        setActiveOption(enabled[next]!);
    }

    async function openRoot(root: SandboxRoot): Promise<void> {
        if (loading || rootIsDisabled(root)) return;
        await openDirectory({ rootId: root.id, relativePath: '' }, root);
    }

    async function openDirectory(reference: DirectoryRef, root = activeRoot): Promise<boolean> {
        if (!root) return false;
        loading = true;
        error = '';
        try {
            const listing = await transport.sandboxDirectory(reference);
            activeRoot = root;
            directory = listing.directory;
            entries = listing.entries;
            nextCursor = listing.nextCursor;
            activeOptionIndex = listing.entries.some(entryIsVisible) ? 0 : -1;
            ondirectorychange?.(listing.directory);
            void revealActiveOption();
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
            const combinedEntries = [...entries, ...listing.entries];
            entries = combinedEntries;
            nextCursor = listing.nextCursor;
            if (activeOptionIndex < 0 && combinedEntries.some(entryIsVisible)) {
                activeOptionIndex = 0;
                void revealActiveOption();
            }
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            loading = false;
        }
    }

    function activate(entry: SandboxEntry): void {
        if (!activeRoot || loading) return;
        const reference = { rootId: activeRoot.id, relativePath: entry.relativePath };
        if (entry.kind === 'DIRECTORY') {
            void openDirectory(reference, activeRoot);
            return;
        }
        onselect(serverFileLocation(reference, `${activeRoot.displayName}/${entry.relativePath}`));
    }

    function activateOption(index: number): void {
        if (loading) return;
        if (!activeRoot) {
            const root = roots[index];
            if (root && !rootIsDisabled(root)) void openRoot(root);
            return;
        }
        const entry = visibleEntries[index];
        if (entry) activate(entry);
    }

    function handleListKeydown(event: KeyboardEvent): void {
        if (loading || event.altKey || event.ctrlKey || event.metaKey) return;
        if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
            event.preventDefault();
            moveActiveOption(event.key === 'ArrowUp' ? -1 : 1);
            return;
        }
        if (event.key === 'Home' || event.key === 'End') {
            event.preventDefault();
            const enabled = enabledOptionIndices();
            const index = event.key === 'Home' ? enabled[0] : enabled.at(-1);
            if (index !== undefined) setActiveOption(index);
            return;
        }
        if (event.key === 'ArrowLeft') {
            if (!activeRoot || !directory?.relativePath) return;
            event.preventDefault();
            void goUp();
            return;
        }
        if (event.key === 'ArrowRight') {
            event.preventDefault();
            if (!activeRoot || visibleEntries[activeOptionIndex]?.kind === 'DIRECTORY') {
                activateOption(activeOptionIndex);
            }
            return;
        }
        if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            activateOption(activeOptionIndex);
        }
    }

    function goHome(): void {
        activeRoot = null;
        directory = null;
        entries = [];
        nextCursor = null;
        error = '';
        selectFirstOption();
        ondirectorychange?.(null);
        void revealActiveOption();
    }

    async function goUp(): Promise<void> {
        if (!activeRoot || !directory?.relativePath || loading) return;
        const parts = directory.relativePath.split('/');
        parts.pop();
        await openDirectory({ rootId: directory.rootId, relativePath: parts.join('/') }, activeRoot);
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
    class="dialog-backdrop storage-picker-backdrop"
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

        <nav class="storage-picker-location" aria-label="Storage location">
            <button
                class="icon-button"
                type="button"
                aria-label="Parent directory"
                disabled={!activeRoot || !directory?.relativePath || loading}
                onclick={() => void goUp()}
            >
                <Icon name="chevron" size={14} class="storage-picker-parent-icon" />
            </button>
            <span
                class="storage-picker-path"
                title={activeRoot && directory
                    ? `${activeRoot.displayName}${directory.relativePath ? `/${directory.relativePath}` : ''}`
                    : 'Workspaces'}
            >
                {activeRoot && directory
                    ? `${activeRoot.displayName}${directory.relativePath ? `/${directory.relativePath}` : ''}`
                    : 'Workspaces'}
            </span>
            <div class="storage-picker-location-actions">
                {#if activeRoot?.writable && directory && mode !== 'file'}
                    <button
                        class="secondary-button storage-picker-directory-action"
                        type="button"
                        disabled={loading}
                        onclick={beginCreateDirectory}
                    >
                        <Icon name="folder-plus" size={14} />
                        New folder
                    </button>
                {/if}
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Go to all workspaces"
                    title="All workspaces"
                    disabled={!activeRoot || loading}
                    onclick={goHome}
                >
                    <Icon name="home" size={15} />
                </button>
            </div>
        </nav>

        <div
            bind:this={listElement}
            class="storage-picker-list"
            role="listbox"
            aria-label="Storage entries"
            aria-activedescendant={activeOptionId}
            aria-busy={loading}
            tabindex="0"
            onkeydown={handleListKeydown}
        >
            {#if !activeRoot}
                {#each roots as root, index (root.id)}
                    <button
                        id={`storage-picker-option-root-${index}`}
                        class:storage-picker-row-active={activeOptionIndex === index}
                        class="storage-picker-row"
                        type="button"
                        role="option"
                        aria-selected={activeOptionIndex === index}
                        aria-disabled={loading || rootIsDisabled(root)}
                        data-picker-index={index}
                        tabindex="-1"
                        disabled={loading || rootIsDisabled(root)}
                        onclick={() => {
                            setActiveOption(index);
                            void openRoot(root);
                        }}
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
                {#each visibleEntries as entry, index (`${entry.kind}:${entry.relativePath}`)}
                    <button
                        id={`storage-picker-option-entry-${index}`}
                        class:storage-picker-file-row={entry.kind === 'FILE'}
                        class:storage-picker-row-active={activeOptionIndex === index}
                        class="storage-picker-row"
                        type="button"
                        role="option"
                        aria-selected={activeOptionIndex === index}
                        aria-disabled={loading}
                        data-picker-index={index}
                        tabindex="-1"
                        disabled={loading}
                        onclick={() => {
                            setActiveOption(index);
                            activate(entry);
                        }}
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
