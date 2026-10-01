<script lang="ts">
    import { onDestroy, tick } from 'svelte';
    import type { FilesystemEntry, FilesystemMutationDriver } from '../../lib/filesystem';
    import Icon from '../../lib/components/Icon.svelte';
    import ObjectContextMenu from '../../lib/components/ObjectContextMenu.svelte';
    import type { FilesController, FilesContext } from './controller.svelte';
    import { FilesEditWorkflow } from './editWorkflow.svelte';
    import FilesEditDialog from './FilesEditDialog.svelte';
    import { moveTargetAllowed } from './moveSelection';
    import type {
        FilesystemDropReader,
        FilesystemImportActions,
        FilesystemImageImporter,
    } from '../../lib/filesystemImport';
    import { FilesImportWorkflow } from './importWorkflow.svelte';
    import FilesImportDialog from './FilesImportDialog.svelte';
    import { FilesImageImportWorkflow, isFilesystemImageDrop } from './imageImportWorkflow.svelte';
    import FilesImageImportDialog from './FilesImageImportDialog.svelte';
    let {
        controller,
        driver,
        imports,
        onexport,
        exportBlocked = false,
        imageImport,
        setStatus = () => undefined,
    }: {
        controller: FilesController;
        driver?: FilesystemMutationDriver;
        imports?: FilesystemImportActions;
        onexport?: (entries: FilesystemEntry[]) => void;
        exportBlocked?: boolean;
        imageImport?: FilesystemImageImporter;
        setStatus?: (message: string) => void;
    } = $props();
    const workflow = new FilesEditWorkflow(
        (review, name, revision) => controller.recordRename(review.revision, review.entries[0], name, revision),
        (message) => {
            setStatus(message);
            void tick().then(() => {
                const workspace = toolbar?.closest('[data-navigation-workspace]');
                const row = [...(workspace?.querySelectorAll<HTMLElement>('[data-file-entry]') ?? [])].find(
                    (entry) => entry.dataset.fileEntry === controller.focused?.id,
                );
                row?.focus({ preventScroll: true });
            });
        },
        (review, revision) => controller.recordMove(review.revision, review.entries, review.destination!, revision),
    );
    let toolbar: HTMLDivElement;
    const importer = new FilesImportWorkflow((message) => setStatus(message));
    const imageFiles = new FilesImageImportWorkflow((message) => setStatus(message));
    const importing = $derived(!!importer.target || !!imageFiles.importer.target);
    let resolving = $state(false);
    let menu = $state<{ left: number; top: number } | null>(null);
    const createTarget = $derived(controller.selection[0] ?? controller.root);
    const destinationAvailable = $derived(
        !!createTarget &&
            (createTarget.kind === 'file'
                ? !!createTarget.parentId
                : !createTarget.filesystemMetadata && !createTarget.issue),
    );
    const exportTargets = $derived(
        controller.selection.length ? controller.selection : controller.root ? [controller.root] : [],
    );
    const canExport = $derived(
        !!onexport &&
            !exportBlocked &&
            !workflow.review &&
            !importing &&
            controller.revision > 0 &&
            exportTargets.length > 0 &&
            exportTargets.every((entry) => !entry.filesystemMetadata && !entry.issue),
    );
    const canCreate = $derived(
        !!driver &&
            !!controller.capabilities?.createDirectory &&
            !resolving &&
            !workflow.review &&
            !importing &&
            !exportBlocked &&
            controller.selection.length <= 1 &&
            destinationAvailable,
    );
    const canDelete = $derived(
        !!driver &&
            !!controller.capabilities?.deleteEntry &&
            !workflow.review &&
            !importing &&
            !exportBlocked &&
            controller.selection.length > 0 &&
            controller.selection.every((entry) => !!entry.parentId && !entry.filesystemMetadata && !entry.issue),
    );
    const canRename = $derived(
        !!driver &&
            !!controller.capabilities?.renameEntry &&
            !resolving &&
            !workflow.review &&
            !importing &&
            !imageImport?.busy &&
            !exportBlocked &&
            controller.selection.length === 1 &&
            controller.selection.every(
                (entry) =>
                    !!entry.parentId &&
                    (entry.kind === 'file' || entry.kind === 'directory') &&
                    !entry.filesystemMetadata &&
                    !entry.issue &&
                    !entry.attributes.some((attribute) => attribute.code === 'fat.read-only'),
            ),
    );
    const canImport = $derived(
        !!imports &&
            !!driver &&
            !!controller.capabilities?.putFile &&
            !resolving &&
            !workflow.review &&
            !importing &&
            !exportBlocked &&
            controller.selection.length <= 1 &&
            destinationAvailable,
    );
    onDestroy(() => {
        workflow.dispose();
        importer.dispose();
        imageFiles.dispose();
    });

    async function openDestination(kind: 'create' | 'import' | 'directory-import'): Promise<void> {
        if (!(kind === 'create' ? canCreate : canImport) || !driver || !controller.capabilities) return;
        const current = controller;
        const revision = current.revision;
        const selected = createTarget;
        if (!selected) return;
        const capabilities = current.capabilities!;
        const boundDriver = driver;
        const boundImports = imports;
        resolving = true;
        try {
            const target =
                selected.kind === 'file'
                    ? selected.parentId === current.rootId
                        ? current.root
                        : await current.lookup({ entryId: selected.parentId! })
                    : selected;
            if (!target || target.kind === 'file' || target.filesystemMetadata || target.issue)
                throw new Error('The destination directory is not writable.');
            if (target && controller === current && current.revision === revision) {
                if (kind === 'create')
                    workflow.open({ kind: 'create', revision, entries: [target], capabilities }, boundDriver);
                else if (
                    boundImports &&
                    importer.open(revision, target, capabilities, boundImports, boundDriver) &&
                    kind === 'directory-import'
                )
                    await importer.chooseDirectory();
            }
        } catch (error) {
            current.error = error instanceof Error ? error.message : String(error);
        } finally {
            resolving = false;
        }
    }

    export function deleteSelection(): void {
        if (canDelete && driver && controller.capabilities)
            workflow.open(
                {
                    kind: 'delete',
                    revision: controller.revision,
                    entries: controller.selection,
                    capabilities: controller.capabilities,
                },
                driver,
            );
    }

    export function renameSelection(): void {
        if (canRename && driver && controller.capabilities) {
            const boundDriver = driver;
            const current = controller;
            let context: FilesContext | undefined;
            workflow.open(
                {
                    kind: 'rename',
                    revision: controller.revision,
                    entries: controller.selection,
                    capabilities: controller.capabilities,
                },
                {
                    ...boundDriver,
                    refresh: async () => {
                        context ??= current.capture();
                        await boundDriver.refresh();
                        await tick();
                        await controller.initialize(context);
                        if (!controller.initialized || controller.error)
                            throw new Error(controller.error || 'Filesystem refresh failed');
                    },
                },
            );
        }
    }

    export function isBusy(): boolean {
        return (
            resolving || !!workflow.review || !!importer.target || !!imageFiles.importer.target || !!imageImport?.busy
        );
    }

    export function canMove(entries: FilesystemEntry[], target: FilesystemEntry | null): boolean {
        return (
            !!driver &&
            !!controller.capabilities?.moveEntry &&
            !controller.busy &&
            !isBusy() &&
            !exportBlocked &&
            moveTargetAllowed(entries, target)
        );
    }

    export async function move(entries: FilesystemEntry[], target: FilesystemEntry): Promise<void> {
        if (!canMove(entries, target) || !driver || !controller.capabilities) return;
        const current = controller;
        const revision = current.revision;
        const boundDriver = driver;
        let context: FilesContext | undefined;
        await workflow.openMove(
            { kind: 'move', revision, entries, destination: target, capabilities: current.capabilities! },
            {
                ...boundDriver,
                refresh: async () => {
                    context ??= current.capture();
                    await boundDriver.refresh();
                    await tick();
                    await controller.initialize(context);
                    if (!controller.initialized || controller.error)
                        throw new Error(controller.error || 'Filesystem refresh failed');
                },
            },
            () => current.reviewChildren(target.id, revision),
        );
    }

    export function canDrop(target: FilesystemEntry | null): boolean {
        return (
            !!target &&
            target.rootId === controller.rootId &&
            ((!target.filesystemMetadata && !target.issue && (target.kind !== 'file' || !!target.parentId)) ||
                !!imageImport?.enabled) &&
            !controller.busy &&
            !isBusy() &&
            !exportBlocked &&
            !!driver &&
            !!imports?.supportsClientUploads &&
            (!!controller.capabilitiesFor(target.rootId)?.putFile || !!imageImport?.enabled)
        );
    }

    export async function drop(target: FilesystemEntry, read: FilesystemDropReader): Promise<void> {
        if (!canDrop(target) || !driver || !imports) return;
        const current = controller;
        const revision = current.revision;
        const boundDriver = driver;
        const boundImports = imports;
        const boundImageImport = imageImport;
        const unchanged = () => controller === current && current.revision === revision;
        menu = null;
        const fatImages =
            !!boundImports.images &&
            !!current.capabilitiesFor(target.rootId)?.supportedImports.includes('FAT_FLOPPY_CONTENTS');
        if (boundImageImport?.enabled || fatImages) {
            resolving = true;
            try {
                const entries = await read(new AbortController().signal, () => undefined);
                if (!unchanged()) return;
                if (fatImages && isFilesystemImageDrop(entries)) {
                    const destination =
                        target.kind === 'file'
                            ? target.parentId === current.rootId
                                ? current.root
                                : await current.lookup({ entryId: target.parentId! })
                            : target;
                    if (destination && unchanged())
                        await imageFiles.open(
                            revision,
                            destination,
                            current.capabilitiesFor(target.rootId)!,
                            boundImports,
                            boundDriver,
                            entries,
                        );
                    return;
                }
                if (boundImageImport?.enabled && (await boundImageImport.open(entries, target))) return;
                if (!unchanged()) return;
                read = async () => entries;
            } catch (error) {
                current.error = error instanceof Error ? error.message : String(error);
                return;
            } finally {
                resolving = false;
            }
        }
        if (target.kind === 'file') {
            const parent =
                target.parentId === current.rootId ? current.root : await current.lookup({ entryId: target.parentId! });
            if (!parent || parent.issue || parent.filesystemMetadata) return;
            target = parent;
        }
        if (!unchanged()) return;
        const capabilities = current.capabilitiesFor(target.rootId)!;
        if (!capabilities?.putFile) {
            current.error = 'Raw file import is not available for this filesystem.';
            return;
        }
        if (importer.open(revision, target, capabilities, boundImports, boundDriver))
            await importer.chooseDropped(read);
    }

    export async function importRoot(root: FilesystemEntry): Promise<void> {
        const capabilities = controller.capabilitiesFor(root.id);
        if (
            isBusy() ||
            exportBlocked ||
            !imports ||
            !driver ||
            !capabilities?.createDirectory ||
            !controller.roots.some((entry) => entry.id === root.id)
        )
            return;
        if (importer.open(controller.revision, root, capabilities, imports, driver)) await importer.chooseDirectory();
    }

    export function openMenu(event: MouseEvent): void {
        event.preventDefault();
        if (canCreate || canImport || canDelete || canRename || canExport)
            menu = { left: event.clientX, top: event.clientY };
    }
</script>

<div class="files-actions" bind:this={toolbar}>
    <span class="files-selection-count" role="status"
        >{controller.selection.length ? `${controller.selection.length} selected` : ''}</span
    >
    <button
        class="icon-button"
        type="button"
        title="Clear selection"
        aria-label="Clear selection"
        disabled={!controller.selection.length}
        onclick={() => controller.clearSelection()}><Icon name="close" size={14} /></button
    >
    <button
        class="icon-button"
        type="button"
        title="New directory..."
        aria-label="New directory..."
        disabled={!canCreate}
        onclick={() => void openDestination('create')}><Icon name="folder-plus" size={14} /></button
    >
    {#if imports}<button
            class="icon-button"
            type="button"
            title="Add files..."
            aria-label="Add files..."
            disabled={!canImport}
            onclick={() => void openDestination('import')}><Icon name="file-plus" size={14} /></button
        >{/if}
    <button
        class="icon-button"
        type="button"
        title="Rename..."
        aria-label="Rename..."
        disabled={!canRename}
        onclick={renameSelection}><Icon name="rename" size={14} /></button
    >
    <button
        class="icon-button"
        type="button"
        title="Delete selected entries..."
        aria-label="Delete selected entries..."
        disabled={!canDelete}
        onclick={deleteSelection}><Icon name="trash" size={14} /></button
    >
    {#if onexport}
        <button
            class="icon-button"
            type="button"
            title="Export to disk..."
            aria-label="Export to disk..."
            disabled={!canExport}
            onclick={() => onexport?.(exportTargets)}><Icon name="archive" size={14} /></button
        >
    {/if}
</div>
{#if menu}
    <ObjectContextMenu
        objectName={controller.selected?.name ?? controller.root?.name ?? 'Files'}
        left={menu.left}
        top={menu.top}
        selectionNoun="entries"
        selectionCount={controller.selection.length}
        oncreatedirectory={canCreate ? () => void openDestination('create') : undefined}
        onimportfiles={canImport ? () => void openDestination('import') : undefined}
        onimportdirectory={canImport && controller.capabilities?.createDirectory
            ? () => void openDestination('directory-import')
            : undefined}
        additionalImports={imageImport?.enabled && !imageImport.busy && !exportBlocked
            ? [
                  {
                      label: imageImport.label,
                      action: () =>
                          void imageImport?.open(undefined, controller.selected ?? controller.root ?? undefined),
                  },
              ]
            : []}
        ondelete={canDelete ? deleteSelection : undefined}
        onrename={canRename ? renameSelection : undefined}
        onexportfiles={canExport ? () => onexport?.(exportTargets) : undefined}
        onclose={() => (menu = null)}
    />
{/if}
{#if workflow.review}<FilesEditDialog {workflow} />{/if}
<FilesImportDialog workflow={importer} />
<FilesImageImportDialog workflow={imageFiles} />

<style>
    .files-selection-count {
        font-size: 10px;
        color: var(--color-text-muted);
        min-width: 64px;
        text-align: right;
        white-space: nowrap;
    }
    .files-actions {
        display: flex;
        flex: none;
        align-items: center;
        gap: 4px;
        margin-left: auto;
    }
</style>
