<script lang="ts">
    import { onMount, tick } from 'svelte';
    import type { ImageTransport } from '../transport';
    import { userFacingMessage } from '../userFacingMessage';
    import { modal } from '../modal';
    import {
        serverDirectoryLocation,
        serverFileLocation,
        type DirectoryRef,
        type FileRef,
        type FileLocation,
        type ImageLocation,
        type DirectoryLocation,
        type SandboxEntry,
        type SandboxRoot,
    } from '../storageLocations';
    import { restoreRememberedFile, storageEntryIsVisible } from '../storagePickerRestoration';
    import Icon from './Icon.svelte';
    import StoragePickerFooter from './StoragePickerFooter.svelte';
    import StoragePickerLocation from './StoragePickerLocation.svelte';

    import type { PickerMode } from '../../features/dialogs/picker';
    interface Props {
        transport: ImageTransport;
        mode: PickerMode;
        active?: boolean;
        extensions?: string[];
        suggestedName?: string;
        multiple?: boolean;
        sourceSelection?: {
            references: readonly DirectoryRef[];
            ontoggle: (location: FileLocation | DirectoryLocation) => void;
        };
        initialDirectory?: DirectoryRef | null;
        initialFile?: FileRef | null;
        requireWritableDirectory?: boolean;
        ondirectorychange?: (directory: DirectoryRef | null) => void;
        onmanagelocations?: () => void;
        onselect: (selection: Exclude<ImageLocation, { kind: 'device' }> | DirectoryLocation) => void;
        onselectmany?: (selections: FileLocation[]) => void;
        onselectsources?: (selections: ImageLocation[]) => void;
        oncancel: () => void;
    }

    let {
        transport,
        mode,
        active = true,
        extensions = [],
        suggestedName = '',
        multiple = false,
        sourceSelection,
        initialDirectory = null,
        initialFile = null,
        requireWritableDirectory = true,
        ondirectorychange,
        onmanagelocations,
        onselect,
        onselectmany,
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
    let selectedFilePaths = $state<string[]>([]);
    let openingCurrentFolder = $state(false);

    const normalizedExtensions = $derived(extensions.map((extension) => extension.toLocaleLowerCase()));
    const visibleEntries = $derived(entries.filter(entryIsVisible));
    const selectedFilePathSet = $derived(
        new Set(
            sourceSelection
                ? sourceSelection.references
                      .filter((reference) => reference.rootId === activeRoot?.id)
                      .map((reference) => reference.relativePath)
                : selectedFilePaths,
        ),
    );
    const multiselect = $derived(multiple || !!sourceSelection);
    const activeOptionId = $derived(
        activeOptionIndex >= 0
            ? `storage-picker-option-${activeRoot ? 'entry' : 'root'}-${activeOptionIndex}`
            : undefined,
    );

    onMount(async () => {
        outputName = suggestedName;
        try {
            roots = await transport.sandboxRoots();
            selectFirstOption();
            if (initialDirectory) {
                const root = roots.find((candidate) => candidate.id === initialDirectory.rootId);
                if (root) {
                    if (!(await openDirectory(initialDirectory, root, initialFile))) ondirectorychange?.(null);
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
        return storageEntryIsVisible(
            entry,
            mode === 'file' || mode === 'media-source' || mode === 'floppy-source',
            normalizedExtensions,
        );
    }

    function rootIsDisabled(root: SandboxRoot): boolean {
        return (
            (mode === 'save-file' || mode === 'save-directory' || (mode === 'directory' && requireWritableDirectory)) &&
            !root.writable
        );
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

    async function openDirectory(
        reference: DirectoryRef,
        root = activeRoot,
        rememberedFile: FileRef | null = null,
    ): Promise<boolean> {
        if (!root) return false;
        loading = true;
        error = '';
        try {
            const restored = await restoreRememberedFile(
                await transport.sandboxDirectory(reference),
                rememberedFile,
                entryIsVisible,
                (cursor) => transport.sandboxDirectory(reference, cursor),
            );
            const listing = restored.listing;
            activeRoot = root;
            directory = listing.directory;
            entries = listing.entries;
            nextCursor = listing.nextCursor;
            selectedFilePaths =
                !sourceSelection && multiple && restored.activeOptionIndex !== null && rememberedFile
                    ? [rememberedFile.relativePath]
                    : [];
            activeOptionIndex = restored.activeOptionIndex ?? (listing.entries.some(entryIsVisible) ? 0 : -1);
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

    async function activate(entry: SandboxEntry): Promise<void> {
        if (!activeRoot || loading) return;
        const reference = { rootId: activeRoot.id, relativePath: entry.relativePath };
        if (entry.kind === 'DIRECTORY') {
            await openDirectory(reference, activeRoot);
            return;
        }
        if (sourceSelection) {
            sourceSelection.ontoggle(serverFileLocation(reference, `${activeRoot.displayName}/${entry.relativePath}`));
            return;
        }
        if (multiple) {
            selectedFilePaths = selectedFilePathSet.has(entry.relativePath)
                ? selectedFilePaths.filter((path) => path !== entry.relativePath)
                : [...selectedFilePaths, entry.relativePath];
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
        if (entry) void activate(entry);
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
            if (!activeRoot) return;
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
        selectedFilePaths = [];
        error = '';
        selectFirstOption();
        ondirectorychange?.(null);
        void revealActiveOption();
    }

    async function goUp(): Promise<void> {
        if (!activeRoot || !directory || loading) return;
        if (!directory.relativePath) {
            goHome();
            return;
        }
        const parts = directory.relativePath.split('/');
        parts.pop();
        await openDirectory({ rootId: directory.rootId, relativePath: parts.join('/') }, activeRoot);
    }

    async function openBreadcrumb(reference: DirectoryRef | null): Promise<void> {
        if (!reference || !activeRoot || loading) return;
        await openDirectory(reference, activeRoot);
    }

    function selectCurrentDirectory(): void {
        if (!activeRoot || !directory) return;
        (sourceSelection?.ontoggle ?? onselect)(
            serverDirectoryLocation(
                directory,
                directory.relativePath ? `${activeRoot.displayName}/${directory.relativePath}` : activeRoot.displayName,
            ),
        );
    }

    async function openCurrentMediaSource(): Promise<void> {
        if (!activeRoot || !directory || loading || openingCurrentFolder) return;
        openingCurrentFolder = true;
        error = '';
        try {
            const kind = await transport.inspectSandboxMediaSource(directory);
            if (kind !== 'DIRECTORY') {
                error = 'Current folder is not a recognized sampler image source';
                return;
            }
            onselect(
                serverDirectoryLocation(
                    directory,
                    directory.relativePath
                        ? `${activeRoot.displayName}/${directory.relativePath}`
                        : activeRoot.displayName,
                ),
            );
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            openingCurrentFolder = false;
        }
    }

    function selectFiles(): void {
        if (!activeRoot || selectedFilePaths.length === 0 || !onselectmany) return;
        const selected = visibleEntries.flatMap((entry) =>
            entry.kind === 'FILE' && selectedFilePathSet.has(entry.relativePath)
                ? [
                      serverFileLocation(
                          { rootId: activeRoot!.id, relativePath: entry.relativePath },
                          `${activeRoot!.displayName}/${entry.relativePath}`,
                      ),
                  ]
                : [],
        );
        if (selected.length > 0) onselectmany(selected);
    }

    function selectOutput(): void {
        if (!activeRoot || !directory) return;
        const filename = outputName.trim();
        if (!filename || filename === '.' || filename === '..' || filename.includes('/') || filename.includes('\\')) {
            error =
                mode === 'save-directory'
                    ? 'Enter a folder name without directory separators'
                    : 'Enter a filename without directory separators';
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
        const reference = { rootId: directory.rootId, relativePath };
        onselect(
            mode === 'save-directory'
                ? serverDirectoryLocation(reference, `${activeRoot.displayName}/${relativePath}`)
                : serverFileLocation(reference, `${activeRoot.displayName}/${relativePath}`),
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

<StoragePickerLocation
    root={activeRoot}
    {directory}
    {loading}
    folderSelected={!!directory && selectedFilePathSet.has(directory.relativePath)}
    onhome={goHome}
    onup={() => void goUp()}
    onbreadcrumb={(reference) => void openBreadcrumb(reference)}
    oncreate={activeRoot?.writable && (mode === 'directory' || mode === 'save-file' || mode === 'save-directory')
        ? beginCreateDirectory
        : undefined}
    ontogglefolder={sourceSelection ? selectCurrentDirectory : undefined}
/>

<div class="storage-picker-results" class:source-selection={!!sourceSelection}>
    <div
        bind:this={listElement}
        class="storage-picker-list"
        role="listbox"
        aria-label="Storage entries"
        aria-multiselectable={multiselect || undefined}
        aria-activedescendant={activeOptionId}
        aria-busy={loading}
        tabindex="0"
        data-dialog-initial-focus={active ? '' : undefined}
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
                    aria-selected={multiselect && entry.kind === 'FILE'
                        ? selectedFilePathSet.has(entry.relativePath)
                        : activeOptionIndex === index}
                    aria-disabled={loading}
                    data-picker-index={index}
                    tabindex="-1"
                    disabled={loading}
                    onclick={() => {
                        setActiveOption(index);
                        void activate(entry);
                    }}
                >
                    {#if entry.kind === 'DIRECTORY'}
                        <Icon name="folder" size={16} />
                    {:else if multiselect}
                        <span
                            class:storage-picker-selection-checked={selectedFilePathSet.has(entry.relativePath)}
                            class="storage-picker-selection"
                        >
                            {#if selectedFilePathSet.has(entry.relativePath)}<Icon name="check" size={12} />{/if}
                        </span>
                    {/if}
                    <span>
                        <strong>{entry.name}</strong>
                        <small>
                            {#if entry.kind === 'DIRECTORY'}
                                Folder
                            {:else if mode === 'media-source'}
                                Disk image · {entry.size ?? 0} bytes
                            {:else}
                                {entry.size ?? 0} bytes
                            {/if}
                        </small>
                    </span>
                    {#if entry.kind === 'DIRECTORY'}
                        <Icon name="chevron" size={14} />
                    {/if}
                </button>
            {/each}
            {#if visibleEntries.length === 0 && !loading}<p class="empty-copy">No matching entries</p>{/if}
        {/if}
        {#if loading}<p class="empty-copy">Loading</p>{/if}
    </div>
    {#if sourceSelection && nextCursor}
        <button class="secondary-button load-more" type="button" disabled={loading} onclick={() => void loadMore()}
            >Load more</button
        >
    {/if}
</div>

{#if error}<p class="storage-picker-error" role="alert">{error}</p>{/if}

{#if !sourceSelection}
    <StoragePickerFooter
        {mode}
        hasDirectory={!!directory}
        hasMore={!!nextCursor}
        {multiple}
        {loading}
        opening={openingCurrentFolder}
        selectedCount={selectedFilePaths.length}
        bind:outputName
        onloadmore={() => void loadMore()}
        onoutput={selectOutput}
        ondirectory={selectCurrentDirectory}
        onmedia={() => void openCurrentMediaSource()}
        onfiles={selectFiles}
        {oncancel}
    />
{/if}

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
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Close"
                    disabled={entryActionBusy}
                    onclick={closeEntryAction}><Icon name="close" size={15} /></button
                >
            </header>
            <div class="entry-action-content">
                <label>
                    <span>Folder name</span>
                    <input
                        aria-label="Folder name"
                        bind:value={entryActionName}
                        data-dialog-initial-focus="caret"
                        disabled={entryActionBusy}
                    />
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

<style>
    .storage-picker-results {
        display: contents;
    }
    .source-selection {
        display: block;
        flex: 1;
        min-height: 0;
        overflow: auto;
        scrollbar-gutter: stable;
    }
    .source-selection .storage-picker-list {
        overflow: visible;
        scrollbar-gutter: auto;
    }
    .load-more {
        margin: 4px 14px 8px;
    }
</style>
