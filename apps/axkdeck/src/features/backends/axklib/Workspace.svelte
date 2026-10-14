<script lang="ts">
    import { onDestroy, tick, untrack } from 'svelte';
    import WorkspaceShell from '../../workspace/WorkspaceShell.svelte';
    import DocumentSelector from '../../workspace/DocumentSelector.svelte';
    import ASeriesPresentation from '../../devices/a-series/Presentation.svelte';
    import Tx16wPresentation from '../../devices/tx16w/Presentation.svelte';
    import type { DevicePresentationState, WorkspaceMode, WorkspacePresentation } from '../../workspace/contracts';
    import type { WorkspaceProps } from './workspaceProps';
    import { FilesController } from '../../files/controller.svelte';
    import FilesView from './AxklibFilesView.svelte';
    import FilesNavigation from '../../files/FilesNavigation.svelte';
    import FilesInspector from '../../files/FilesInspector.svelte';
    import FilesExportCoordinator from '../../files/FilesExportCoordinator.svelte';
    import ImageActions from '../../../lib/components/ImageActions.svelte';
    import AboutDialog from '../../../lib/components/AboutDialog.svelte';
    import { desktopBuildInfo, type DesktopBuildInfoState } from '../../../lib/desktopBuildInfo';
    import type { FilesystemEntry, FilesystemBinding } from '../../../lib/filesystem';
    import { userFacingMessage } from '../../../lib/userFacingMessage';
    import { ViewMemory, shellViewState } from '../../workspace/viewMemory';

    let { mode = $bindable('files'), inspectorOpen = $bindable(true), ...workspace }: WorkspaceProps = $props();
    const document = $derived(workspace.documents.find((item) => item.id === workspace.documentId));
    const filesDriver = $derived(
        workspace.sessionId !== null ? workspace.filesMutations?.(workspace.sessionId) : undefined,
    );
    const exportDriver = $derived(
        workspace.sessionId !== null ? workspace.filesExports?.(workspace.sessionId) : undefined,
    );
    const importDriver = $derived(
        workspace.sessionId !== null ? workspace.filesImports?.(workspace.sessionId) : undefined,
    );
    let files = $state<FilesController | null>(null);
    let previousSession: number | null = null;
    let fileExporter = $state<FilesExportCoordinator>();
    let fileView = $state<{
        importRoot(root: FilesystemEntry): void;
        importCommands(root: FilesystemEntry): { label: string; action: () => void }[];
        isBusy(): boolean;
    }>();
    let activePresentation = $state<{
        showObject(id: string): Promise<void>;
        contentScopeId?(): string;
        showScope?(id: string): void;
    }>();
    let revealEntry = $state<FilesystemEntry | null>(null);
    let about = $state<DesktopBuildInfoState | null>(null);
    let buildInfo: ReturnType<typeof desktopBuildInfo> | undefined;
    let mappingGeneration = 0;
    let modeGeneration = 0;
    let previousDocument: string | null = null;
    const memory = new ViewMemory();
    const remember = $derived(memory.forDocument(workspace.sessionId, workspace.documentId));
    const rememberFiles = $derived(memory.forDocument(workspace.sessionId, null));
    const exportFiles = (entries: FilesystemEntry[]) => fileExporter?.open(files?.revision ?? 0, entries);
    const fileDiagnostics = $derived(
        workspace.sources.find((source) => source.id === (files?.selected ?? files?.root)?.sourceMemberId)
            ?.diagnostics ?? [],
    );
    onDestroy(() => {
        void workspace.interfaceScaling?.dispose();
    });

    $effect(() => {
        const sessionId = workspace.sessionId;
        const revision = workspace.revision;
        ++modeGeneration;
        if (sessionId === null || !revision) {
            files = null;
            previousSession = null;
            return;
        }
        const context = untrack(() => (previousSession === sessionId ? files?.capture() : undefined));
        previousSession = sessionId;
        const controller = new FilesController({
            inspect: (query) => workspace.transport.filesystem(sessionId, query),
        });
        files = controller;
        void controller.initialize(context);
        return () => controller.dispose();
    });
    $effect(() => {
        const id = workspace.documentId;
        if (id !== previousDocument) {
            previousDocument = id;
            mode = id ? 'device' : 'files';
            ++modeGeneration;
        }
        if (!id) mode = 'files';
    });
    $effect(() => {
        const controller = files;
        const session = workspace.sessionId;
        const documentId = workspace.documentId;
        const objectId = workspace.catalog.inspectorObjectId;
        const token = ++mappingGeneration;
        revealEntry = null;
        if (!controller?.initialized || !documentId || !objectId || document?.formatId !== 'yamaha.a-series') return;
        void controller
            .lookup({ objectId, documentId })
            .then((entry) => {
                if (token === mappingGeneration && workspace.sessionId === session && controller === files)
                    revealEntry = entry;
            })
            .catch(() => undefined);
    });
    async function changeMode(next: WorkspaceMode, synchronizeRoot = true): Promise<void> {
        if (next === mode || (next === 'device' && !document)) return;
        const controller = files;
        const token = ++modeGeneration;
        const documentId = workspace.documentId;
        if (next === 'files') {
            const scope = activePresentation?.contentScopeId?.();
            if (synchronizeRoot && scope && documentId && controller?.initialized) {
                const entry = await controller.lookup({ contentScopeId: scope, documentId }).catch(() => null);
                if (token !== modeGeneration || controller !== files) return;
                if (entry) await controller.chooseRoot(entry.rootId);
            }
            await workspace.audition.stop();
            if (token !== modeGeneration || documentId !== workspace.documentId) return;
            workspace.clearSelection();
        } else if (synchronizeRoot) {
            const binding = controller?.root?.bindings.find(
                (item) => item.documentId === documentId && item.contentScopeId,
            );
            if (binding?.contentScopeId) activePresentation?.showScope?.(binding.contentScopeId);
        }
        mode = next;
    }
    async function revealInFiles(objectId?: string): Promise<void> {
        const controller = files;
        const documentId = workspace.documentId;
        const token = ++modeGeneration;
        if (!controller || !documentId) return;
        const entry = objectId ? await controller.lookup({ objectId, documentId }).catch(() => null) : revealEntry;
        if (!entry || token !== modeGeneration || controller !== files || workspace.documentId !== documentId) return;
        if ((await controller.reveal(entry)) && token === modeGeneration && workspace.documentId === documentId)
            await changeMode('files', false);
    }
    async function showInDevice(binding: FilesystemBinding): Promise<void> {
        if (workspace.documentSwitchBlocked || fileView?.isBusy() || fileExporter?.isOpen()) return;
        const session = workspace.sessionId;
        try {
            await workspace.selectDocument(binding.documentId);
            if (workspace.sessionId !== session || workspace.documentId !== binding.documentId) return;
            mode = 'device';
            await tick();
            if (workspace.sessionId !== session || workspace.documentId !== binding.documentId) return;
            if (binding.objectId) await activePresentation?.showObject(binding.objectId);
            else if (binding.contentScopeId) activePresentation?.showScope?.(binding.contentScopeId);
        } catch (reason) {
            workspace.setStatus(userFacingMessage(reason));
        }
    }
    async function openAbout(): Promise<void> {
        about = { status: 'loading' };
        try {
            const info = await (buildInfo ??= desktopBuildInfo());
            if (about) about = { status: 'ready', buildInfo: info };
        } catch {
            if (about) about = { status: 'error' };
        }
    }
</script>

{#snippet imageActions()}
    <ImageActions
        image={workspace.imageLocation}
        opening={workspace.imageOpening}
        storageLocationsAvailable={workspace.transport.storageMode === 'server'}
        onopen={workspace.openImage}
        oncreate={workspace.createImage}
        onclose={workspace.closeImage}
        onintegrity={workspace.showImageIntegrity}
        onmanagelocations={workspace.manageLocations}
    />
{/snippet}
{#snippet documents()}
    <DocumentSelector
        documents={workspace.documents}
        selected={workspace.documentId}
        discovery={workspace.discovery}
        disabled={workspace.documentSwitchBlocked ||
            workspace.switchingDocument ||
            workspace.imageOpening ||
            Boolean(fileView?.isBusy()) ||
            Boolean(fileExporter?.isOpen())}
        onselect={workspace.selectDocument}
        onresolve={workspace.resolveDocuments}
    />
{/snippet}
{#snippet fileNavigation()}
    {#if files}<FilesNavigation
            controller={files}
            onexport={exportDriver ? exportFiles : undefined}
            onimport={importDriver ? (root) => fileView?.importRoot(root) : undefined}
            additionalImports={(root) => fileView?.importCommands(root) ?? []}
            exportBlocked={fileExporter?.isOpen() || fileView?.isBusy()}
        />{/if}
{/snippet}
{#snippet fileContent()}
    {#if files}{#key workspace.sessionId}<FilesView
                bind:this={fileView}
                controller={files}
                sessionId={workspace.sessionId!}
                documentId={workspace.documentId}
                driver={filesDriver}
                imports={importDriver}
                exports={exportDriver}
                onexport={exportDriver ? exportFiles : undefined}
                exportBlocked={fileExporter?.isOpen()}
                setStatus={workspace.setStatus}
            />{/key}
    {:else}<p class="empty-copy">No source set open</p>{/if}
{/snippet}
{#snippet fileInspector()}
    {#if files}<FilesInspector
            entry={files.selected ?? files.root}
            documents={workspace.documents}
            diagnostics={fileDiagnostics}
            onshowdevice={(binding) => void showInDevice(binding)}
        />{/if}
{/snippet}
{#snippet emptyDevice()}
    <p class="empty-copy">No native document selected</p>
{/snippet}
{#snippet shell(presentation: WorkspacePresentation, state: DevicePresentationState)}
    {#key mode}
        <WorkspaceShell
            {mode}
            {imageActions}
            documentSelector={documents}
            viewState={(mode === 'files' ? rememberFiles : remember)('shell', shellViewState)}
            interfaceScaling={workspace.interfaceScaling}
            isDesktop={workspace.isDesktop}
            bind:inspectorOpen
            lowerOpen={state.lowerOpen}
            onlowerchange={state.setLowerOpen}
            imageName={workspace.imageLocation?.displayName.split(/[\\/]/).at(-1) ?? ''}
            context={document?.name ?? `${workspace.sources.length} sources`}
            status={workspace.switchingDocument
                ? 'Opening native document'
                : mode === 'device'
                  ? workspace.sourceStatus
                  : files?.error || (files?.busy ? 'Reading filesystem' : 'Files')}
            count={mode === 'device' ? `${document?.objectCount ?? 0} objects` : `${files?.rows.length ?? 0} entries`}
            deviceAvailable={Boolean(document)}
            filesAvailable={Boolean(files?.available || files?.error)}
            onmodechange={(value) => void changeMode(value)}
            onabout={openAbout}
            onconnection={workspace.openConnectionSettings}
            presentation={mode === 'device'
                ? presentation
                : { navigation: fileNavigation, content: fileContent, inspector: fileInspector }}
        />
    {/key}
{/snippet}

{#key `${workspace.sessionId}:${workspace.documentId}`}
    {#if document?.formatId === 'yamaha.a-series'}
        <ASeriesPresentation
            bind:this={activePresentation}
            {workspace}
            {remember}
            canRevealFiles={Boolean(revealEntry)}
            onrevealFiles={() => void revealInFiles()}
            children={shell}
        />
    {:else if document?.formatId === 'yamaha.tx16w'}
        <Tx16wPresentation
            bind:this={activePresentation}
            transport={workspace.transport}
            sessionId={workspace.sessionId!}
            revision={workspace.revision ?? 0}
            {document}
            {remember}
            sources={workspace.sources}
            onrevealFiles={(id) => void revealInFiles(id)}
            children={shell}
        />
    {:else}
        {@render shell(
            { navigation: emptyDevice, content: emptyDevice },
            { lowerOpen: false, setLowerOpen: () => undefined, showObject: async () => undefined },
        )}
    {/if}
{/key}
{#if about}<AboutDialog state={about} onclose={() => (about = null)} />{/if}
{#if exportDriver}{#key workspace.sessionId}<FilesExportCoordinator
            driver={exportDriver}
            setStatus={workspace.setStatus}
            bind:this={fileExporter}
        />{/key}{/if}
