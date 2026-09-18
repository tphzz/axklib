<script lang="ts">
    import { onDestroy, onMount, tick, untrack } from 'svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import { collectionPageStep, focusCollectionIndex, linearNavigationIndex } from '../../lib/collectionNavigation';
    import type { FilesController, FileRow } from './controller.svelte';
    import type { FilesystemEntry, FilesystemMutationDriver } from '../../lib/filesystem';
    import FilesActions from './FilesActions.svelte';
    import type {
        FilesystemDropReader,
        FilesystemImportActions,
        FilesystemImageImporter,
    } from '../../lib/filesystemImport';
    import { captureBrowserFilesystemDrop } from '../../lib/browserFilesystemDrop';
    import { nativeFilesystemDrop } from '../../lib/nativeFilesystemDrop';
    import { registerNativeFilesystemDropTarget } from '../../lib/nativeFilesystemDropTarget';
    import { userFacingMessage } from '../../lib/userFacingMessage';
    import { selectionMode } from '../../lib/objectSelection';
    import type { FilesystemExportActions } from '../../lib/filesystemExport';
    import { FilesDragWorkflow } from './dragWorkflow';
    import { FilesMoveGesture } from './moveGesture';
    import { filesBackgroundSelection } from './backgroundSelection';
    let {
        controller,
        driver,
        imports,
        onexport,
        exportBlocked = false,
        exports,
        imageImport,
        setStatus = () => undefined,
    }: {
        controller: FilesController;
        driver?: FilesystemMutationDriver;
        imports?: FilesystemImportActions;
        onexport?: (entries: FilesystemEntry[]) => void;
        exportBlocked?: boolean;
        exports?: FilesystemExportActions;
        imageImport?: FilesystemImageImporter;
        setStatus?: (message: string) => void;
    } = $props();
    let actions = $state<{
        openMenu(event: MouseEvent): void;
        deleteSelection(): void;
        renameSelection(): void;
        importRoot(root: FilesystemEntry): Promise<void>;
        isBusy(): boolean;
        canDrop(target: FilesystemEntry | null): boolean;
        drop(target: FilesystemEntry, read: FilesystemDropReader): Promise<void>;
        canMove(entries: FilesystemEntry[], target: FilesystemEntry | null): boolean;
        move(entries: FilesystemEntry[], target: FilesystemEntry): Promise<void>;
    }>();
    export function importRoot(root: FilesystemEntry): void {
        void actions?.importRoot(root);
    }
    export function isBusy(): boolean {
        return dragBusy || moveActive || (actions?.isBusy() ?? false);
    }
    let scroller: HTMLElement;
    let query = $state('');
    let dropTarget = $state<string | null>(null);
    let dragBusy = $state(false);
    let dragMessage = $state('');
    let dragError = $state('');
    let moveActive = $state(false);
    let moveMessage = $state('');
    const dragWorkflow = new FilesDragWorkflow(
        (message, busy) => {
            dragMessage = message;
            dragBusy = busy;
        },
        (message) => {
            dragError = message;
        },
    );
    const gesture = new FilesMoveGesture({
        controller: () => controller,
        scroller: () => scroller,
        blocked: () => exportBlocked || isBusy(),
        canExport: () => !!exports?.drag && !exportBlocked,
        canMove: (entries, target) => actions?.canMove(entries, target) ?? false,
        move: (entries, target) => {
            void actions?.move(entries, target);
        },
        export: (entries, revision, root) => {
            dragError = '';
            if (exports)
                void dragWorkflow.start(
                    revision,
                    entries,
                    exports,
                    () => controller.revision === revision && controller.rootId === root,
                );
        },
        show: (target, active, count) => {
            moveActive = active;
            dropTarget = target?.id ?? null;
            moveMessage = active
                ? target
                    ? `Move ${count} entries to ${target.path || '/'}`
                    : 'Choose a destination folder'
                : '';
        },
    });
    function releaseDrag(): void {
        gesture.cancel();
        dragWorkflow.cancel();
    }
    let timer: ReturnType<typeof setTimeout> | undefined;
    const rows = $derived(controller.rows);
    const selectedIds = $derived(new Set(controller.selection.map((entry) => entry.id)));
    const moreDirectories = $derived([
        ...(controller.root && controller.hasMore(controller.root.id) ? [controller.root] : []),
        ...rows
            .filter((row) => controller.expanded(row.entry.id) && controller.hasMore(row.entry.id))
            .map((row) => row.entry),
    ]);
    $effect(() => {
        const rootId = controller.rootId;
        if (!controller.viewReady) return;
        clearTimeout(timer);
        query = untrack(() => controller.query);
        const top = untrack(() => controller.scrollTop);
        void tick().then(() => {
            if (scroller && controller.rootId === rootId) scroller.scrollTop = top;
        });
    });
    $effect(() => {
        const sequence = controller.revealSequence;
        if (!sequence) return;
        const id = controller.revealId;
        void tick().then(() => {
            if (!scroller || controller.revealSequence !== sequence) return;
            const target = [...scroller.querySelectorAll<HTMLElement>('[data-file-entry]')].find(
                (row) => row.dataset.fileEntry === id,
            );
            target?.focus({ preventScroll: true });
            target?.scrollIntoView?.({ block: 'nearest', inline: 'nearest', behavior: 'auto' });
        });
    });
    onDestroy(() => clearTimeout(timer));
    onDestroy(() => dragWorkflow.dispose());
    onDestroy(() => gesture.cancel());
    onMount(() =>
        registerNativeFilesystemDropTarget((event, position) => {
            dropTarget = null;
            if (dragBusy || moveActive || !position || !scroller) return;
            const element = document.elementFromPoint(position.x, position.y);
            if (!element || element.closest('[role="dialog"], [role="menu"]')) return;
            if (
                !scroller.contains(element) &&
                !(imageImport?.enabled && scroller.closest('[data-workspace-mode]')?.contains(element))
            )
                return;
            const target = targetAt(element);
            if (!target || !actions?.canDrop(target)) return;
            if (event.type === 'drop') void actions.drop(target, nativeFilesystemDrop(event.paths));
            else dropTarget = target.id;
        }),
    );
    onMount(() => {
        const outside = (event: DragEvent) => {
            if (
                !imageImport?.enabled ||
                !(event.target instanceof Element) ||
                scroller.contains(event.target) ||
                event.target.closest('[role="dialog"], [role="menu"]') ||
                !scroller.closest('[data-workspace-mode]')?.contains(event.target)
            )
                return;
            if (event.type === 'drop') dropped(event);
            else drag(event);
        };
        document.addEventListener('dragover', outside);
        document.addEventListener('drop', outside);
        return () => {
            document.removeEventListener('dragover', outside);
            document.removeEventListener('drop', outside);
        };
    });
    function search(): void {
        clearTimeout(timer);
        timer = setTimeout(() => void controller.search(query), 180);
    }
    function targetAt(element: EventTarget | null): FilesystemEntry | null {
        const id =
            element instanceof Element
                ? element.closest<HTMLElement>('[data-file-entry]')?.dataset.fileEntry
                : undefined;
        return id ? (rows.find((row) => row.entry.id === id)?.entry ?? null) : controller.root;
    }
    function drag(event: DragEvent): void {
        if (dragBusy || moveActive) {
            event.preventDefault();
            return;
        }
        if (!event.dataTransfer || !Array.from(event.dataTransfer.types).includes('Files')) return;
        event.preventDefault();
        event.stopPropagation();
        const target = targetAt(event.target);
        const admitted = actions?.canDrop(target);
        dropTarget = admitted ? target!.id : null;
        event.dataTransfer.dropEffect = admitted ? 'copy' : 'none';
    }
    function dropped(event: DragEvent): void {
        if (dragBusy || moveActive) {
            event.preventDefault();
            return;
        }
        dropTarget = null;
        if (!event.dataTransfer || !Array.from(event.dataTransfer.types).includes('Files')) return;
        event.preventDefault();
        event.stopPropagation();
        const target = targetAt(event.target);
        if (!target || !actions?.canDrop(target)) return;
        try {
            void actions.drop(target, captureBrowserFilesystemDrop(event.dataTransfer));
        } catch (error) {
            controller.error = userFacingMessage(error);
        }
    }
    async function navigate(event: KeyboardEvent, row: FileRow, index: number): Promise<void> {
        const additive = event.ctrlKey || event.metaKey;
        const move = (entry: FileRow['entry']): void => {
            if (event.shiftKey) controller.select(entry, additive ? 'add-range' : 'range');
            else if (additive) controller.focus(entry);
            else controller.select(entry);
        };
        if (event.key === 'Escape') {
            event.preventDefault();
            controller.clearSelection();
            return;
        }
        if (additive && event.key.toLowerCase() === 'a') {
            event.preventDefault();
            controller.select(row.entry, 'all');
            return;
        }
        if (event.key === 'Delete') {
            event.preventDefault();
            if (!controller.selection.length) controller.select(row.entry);
            actions?.deleteSelection();
            return;
        }
        if (event.key === 'F2') {
            event.preventDefault();
            if (!actions?.isBusy()) actions?.renameSelection();
            return;
        }
        if (event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10')) {
            event.preventDefault();
            controller.selectForContext(row.entry);
            const rect = (event.currentTarget as HTMLElement).getBoundingClientRect();
            actions?.openMenu(new MouseEvent('contextmenu', { clientX: rect.left + 24, clientY: rect.top + 24 }));
            return;
        }
        const next = linearNavigationIndex(event.key, index, rows.length, collectionPageStep(event.currentTarget));
        if (next !== null) {
            event.preventDefault();
            move(rows[next].entry);
            await focusCollectionIndex(event.currentTarget, next);
            return;
        }
        if (event.key === 'ArrowRight' && row.entry.kind !== 'file') {
            event.preventDefault();
            if (!controller.expanded(row.entry.id)) await controller.toggle(row.entry);
            else if (rows[index + 1]?.entry.parentId === row.entry.id) {
                move(rows[index + 1].entry);
                await focusCollectionIndex(event.currentTarget, index + 1);
            }
        } else if (event.key === 'ArrowLeft') {
            event.preventDefault();
            if (controller.expanded(row.entry.id)) await controller.toggle(row.entry);
            else {
                const parent = rows.findIndex((item) => item.entry.id === row.entry.parentId);
                if (parent >= 0) {
                    move(rows[parent].entry);
                    await focusCollectionIndex(event.currentTarget, parent);
                }
            }
        } else if (event.key === ' ') {
            event.preventDefault();
            controller.select(row.entry, event.shiftKey ? (additive ? 'add-range' : 'range') : 'toggle');
        } else if (event.key === 'Enter') {
            event.preventDefault();
            controller.selectForContext(row.entry);
            if (row.entry.kind !== 'file') await controller.toggle(row.entry);
        }
    }
</script>

<svelte:window
    onpointermove={(event) => gesture.update(event)}
    onpointerup={(event) => {
        gesture.release(event);
        dragWorkflow.cancel();
    }}
    onpointercancel={releaseDrag}
    onblur={releaseDrag}
    onkeydown={(event) => {
        if (event.key === 'Escape' && (dragBusy || moveActive)) {
            event.preventDefault();
            event.stopPropagation();
            releaseDrag();
        }
    }}
/>

<section
    class="files-workspace"
    aria-label="Files workspace"
    data-navigation-workspace
    data-workspace-background
    use:filesBackgroundSelection={() => {
        if (!isBusy() && !exportBlocked && !gesture.suppressClick) controller.clearSelection();
    }}
>
    <header class="files-toolbar">
        <strong data-files-root-drop class:drop-target={dropTarget === controller.rootId} title="Move to partition root"
            >{controller.root?.name ?? 'Files'}</strong
        >
        <FilesActions
            {setStatus}
            {controller}
            {driver}
            {imports}
            {imageImport}
            {onexport}
            exportBlocked={exportBlocked || dragBusy}
            bind:this={actions}
        />
        <label class="search-field"
            ><Icon name="search" size={14} /><input
                type="search"
                placeholder="Search filesystem"
                aria-label="Search filesystem"
                bind:value={query}
                oninput={search}
            /></label
        >
    </header>
    {#if dragMessage}<div class="files-drag-status" role="status">{dragMessage}</div>{/if}
    {#if moveMessage}<div class="files-move-status" role="status">{moveMessage}</div>{/if}
    {#if dragError}<div class="files-error" role="alert">
            {dragError}<button
                type="button"
                class="icon-button"
                aria-label="Dismiss drag error"
                title="Dismiss drag error"
                onclick={() => (dragError = '')}><Icon name="close" size={12} /></button
            >
        </div>{/if}
    {#if controller.error}<div class="files-error" role="alert">
            {controller.error}<button
                type="button"
                class="secondary-button"
                onclick={() => void controller.initialize()}>Retry</button
            >
        </div>{/if}
    <div
        class="files-scroll"
        data-workspace-background
        class:drop-target={dropTarget === controller.rootId}
        bind:this={scroller}
        ondragenter={drag}
        ondragover={drag}
        ondragleave={(event) => {
            if (!(event.relatedTarget instanceof Node) || !scroller.contains(event.relatedTarget)) dropTarget = null;
        }}
        ondrop={dropped}
        onscroll={() => {
            if (controller.viewReady) controller.scrollTop = scroller.scrollTop;
        }}
        role="treegrid"
        tabindex="-1"
        aria-label="Filesystem entries"
        aria-multiselectable="true"
        aria-busy={controller.busy}
        data-navigation-list
    >
        <div class="files-columns" aria-hidden="true">
            <span>Name</span><span>Kind</span><span>Attributes</span><span>Size</span>
        </div>
        {#each rows as row, index (row.entry.id)}
            <!-- svelte-ignore a11y_no_noninteractive_tabindex a11y_no_noninteractive_element_interactions -->
            <div
                class="file-row"
                class:filesystem-metadata={row.entry.filesystemMetadata}
                class:selected={selectedIds.has(row.entry.id)}
                class:drop-target={dropTarget === row.entry.id}
                role="row"
                aria-level={row.depth + 1}
                aria-selected={selectedIds.has(row.entry.id)}
                aria-expanded={row.entry.kind === 'file' ? undefined : controller.expanded(row.entry.id)}
                tabindex={controller.focused?.id === row.entry.id ||
                (!rows.some((item) => item.entry.id === controller.focused?.id) && index === 0)
                    ? 0
                    : -1}
                data-file-entry={row.entry.id}
                data-navigation-index={index}
                onpointerdown={(event) => gesture.arm(event, row.entry)}
                onclick={(event) => {
                    if (gesture.suppressClick) {
                        gesture.suppressClick = false;
                        return;
                    }
                    controller.select(row.entry, selectionMode(event));
                }}
                oncontextmenu={(event) => {
                    controller.selectForContext(row.entry);
                    (event.currentTarget as HTMLElement).focus();
                    actions?.openMenu(event);
                }}
                onkeydown={(event) => void navigate(event, row, index)}
            >
                <div class="file-name" role="gridcell" style:padding-left={`${6 + row.depth * 16}px`}>
                    {#if row.entry.kind !== 'file'}<button
                            type="button"
                            class="icon-button file-toggle"
                            tabindex="-1"
                            aria-label={`${controller.expanded(row.entry.id) ? 'Collapse' : 'Expand'} ${row.entry.name}`}
                            onclick={(event) => {
                                event.stopPropagation();
                                controller.focus(row.entry);
                                void controller.toggle(row.entry);
                            }}
                            ><span class:expanded={controller.expanded(row.entry.id)}
                                ><Icon name="chevron" size={12} /></span
                            ></button
                        >
                    {:else}<span class="file-toggle"></span>{/if}
                    <Icon name={row.entry.kind === 'file' ? 'file' : 'folder'} size={14} />
                    <span class="file-copy"
                        ><span title={row.entry.name}>{row.entry.name}</span>{#if controller.query}<small
                                title={row.entry.path}>{row.entry.path}</small
                            >{/if}</span
                    >
                    {#if row.entry.issue}<span title={row.entry.issue}><Icon name="triangle-alert" size={12} /></span
                        >{/if}
                </div>
                <span role="gridcell" title={row.entry.filesystemMetadata ? 'Filesystem metadata' : ''}
                    >{row.entry.filesystemMetadata
                        ? 'Metadata'
                        : row.entry.kind === 'file'
                          ? 'File'
                          : 'Directory'}</span
                >
                <span
                    role="gridcell"
                    class="file-attributes"
                    title={row.entry.attributes
                        .map((attribute) => attribute.summary)
                        .filter(Boolean)
                        .join('; ')}
                    >{row.entry.attributes
                        .map((attribute) => attribute.summary)
                        .filter(Boolean)
                        .join(', ') || '-'}</span
                >
                <span role="gridcell"
                    >{row.entry.sizeBytes === null ? '-' : `${row.entry.sizeBytes.toLocaleString()} B`}</span
                >
            </div>
        {/each}
        {#if !rows.length && !controller.busy}<p class="empty-copy">
                {controller.query ? 'No matching entries' : 'No entries'}
            </p>{/if}
        {#if controller.query && controller.moreResults}<button
                class="secondary-button more-entries"
                type="button"
                onclick={() => void controller.search(controller.query, true)}>More results</button
            >{/if}
        {#if !controller.query}{#each moreDirectories as directory (directory.id)}<button
                    class="secondary-button more-entries"
                    type="button"
                    onclick={() => void controller.loadChildren(directory.id)}>More in {directory.name}</button
                >{/each}{/if}
    </div>
</section>

<style>
    .files-workspace {
        position: relative;
        display: flex;
        flex-direction: column;
        min-height: 0;
        min-width: 0;
    }
    .files-toolbar {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        min-height: 36px;
        padding: 4px 8px;
        border-bottom: 1px solid var(--color-border);
    }
    .files-toolbar strong {
        font-size: 11px;
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .search-field {
        width: 180px;
        flex-shrink: 0;
    }
    .files-columns,
    .file-row {
        user-select: none;
        display: grid;
        grid-template-columns: minmax(160px, 1fr) 70px 130px 100px;
        min-width: 460px;
        align-items: center;
    }
    .files-columns {
        position: sticky;
        top: 0;
        z-index: 1;
        padding-block: 7px;
        background: var(--color-bg);
        font-size: 10px;
        color: var(--color-text-muted);
    }
    .files-columns > :first-child {
        padding-left: 6px;
    }
    .files-columns > :last-child {
        padding-right: 6px;
    }
    .files-scroll {
        flex: 1;
        min-height: 0;
        overflow: auto;
        padding: 0 calc(8px + var(--overlay-scrollbar-clearance)) 8px 8px;
        scrollbar-gutter: stable;
    }
    .file-row {
        min-height: 28px;
        border-bottom: 1px solid var(--color-border);
        font-size: 11px;
        outline-offset: -1px;
        cursor: default;
    }
    .file-row.selected {
        background: #263f50;
        outline: 1px solid #497590;
        border-radius: 4px;
    }
    .file-row.drop-target,
    [data-files-root-drop].drop-target,
    .files-scroll.drop-target {
        outline: 1px solid var(--workspace-accent);
        outline-offset: -1px;
        background-color: rgb(93 190 184 / 10%);
    }
    .filesystem-metadata .file-name {
        color: var(--color-text-muted);
    }
    .file-attributes {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .file-row:hover {
        background-color: rgb(104 151 187 / 10%);
    }
    .file-row > span {
        font-size: 10px;
        color: var(--color-text-muted);
        padding-right: 6px;
    }
    .files-columns > :last-child,
    .file-row > :last-child {
        text-align: right;
    }
    .file-name {
        display: flex;
        align-items: center;
        gap: 6px;
        min-width: 0;
        padding-block: 4px;
    }
    .file-toggle {
        display: inline-flex;
        justify-content: center;
        align-items: center;
        width: 16px;
        height: 18px;
        flex: 0 0 16px;
        padding: 0;
    }
    .file-toggle > span {
        display: flex;
    }
    .file-toggle .expanded {
        transform: rotate(90deg);
    }
    .file-copy {
        display: grid;
        min-width: 0;
    }
    .file-copy > * {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .file-copy small {
        font-size: 9px;
        color: var(--color-text-muted);
    }
    .files-error {
        padding: 8px;
        color: #e0b765;
        display: flex;
        justify-content: space-between;
    }
    .files-drag-status {
        font-size: 10px;
        color: var(--color-text-muted);
        padding: 4px 8px;
    }
    .files-move-status {
        position: absolute;
        z-index: 2;
        bottom: 8px;
        left: 8px;
        max-width: calc(100% - 16px);
        overflow: hidden;
        white-space: nowrap;
        text-overflow: ellipsis;
        pointer-events: none;
        background: var(--color-bg);
        border: 1px solid var(--color-border);
        padding: 4px 8px;
        font-size: 10px;
    }
    .more-entries {
        display: block;
        margin: 6px 0;
    }
</style>
