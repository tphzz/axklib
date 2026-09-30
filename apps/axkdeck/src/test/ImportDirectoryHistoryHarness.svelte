<script lang="ts">
    import { createObjectImports } from '../features/import/createObjectImports';
    import { PackagePickerHistory } from '../features/import/packagePickerHistory';
    import { PickerController, type PickerRequest } from '../features/dialogs/picker';
    import PickerDialogHost from '../features/dialogs/PickerDialogHost.svelte';
    import { JobController } from '../features/jobs/actions';
    import FloppyImportDialog from '../lib/components/FloppyImportDialog.svelte';
    import ImageTreeContextMenu from '../lib/components/ImageTreeContextMenu.svelte';
    import type { FloppyInspection } from '../lib/floppyImport';
    import type { DirectoryRef, SandboxEntry } from '../lib/storageLocations';
    import type { ImageTransport } from '../lib/transport';
    import type { DiskTreeItem, ImageTreeAction } from '../lib/types';

    let pickerRequest = $state<PickerRequest | null>(null);
    let menu = $state<{ item: DiskTreeItem; left: number; top: number } | null>(null);
    let missing = $state(false);
    const volume: DiskTreeItem = { id: 'v', name: 'Existing', kind: 'volume', partitionIndex: 0, childCount: 0 };
    const partition: DiskTreeItem = {
        id: 'p',
        name: 'Partition',
        kind: 'partition',
        partitionIndex: 0,
        childCount: 1,
        children: [volume],
    };
    const picker = new PickerController((request) => {
        pickerRequest = request;
    });
    const inspection: FloppyInspection = {
        format: 'A_SERIES',
        inspectionToken: 'fixture',
        complete: false,
        label: 'Kit',
        nextRequiredIndex: 2,
        members: [{ index: 1, label: 'Kit' }],
        excludedFiles: [],
        issues: [],
        objects: [],
    };
    const job = { jobId: 1, kind: 'images.floppy_import.inspect', status: 'completed' as const, result: inspection };
    const transport = {
        connectionMode: 'local',
        sandboxRoots: async () => [{ id: 'test', displayName: 'Test sources', writable: false }],
        sandboxDirectory: async (directory: DirectoryRef) => {
            if (missing && directory.relativePath.startsWith('Disks'))
                throw new Error('Source folder no longer exists');
            const names =
                directory.relativePath === ''
                    ? ['Disks', 'Packages']
                    : directory.relativePath === 'Disks'
                      ? ['Kit']
                      : ['disk1.img', 'disk2.img'];
            const entries: SandboxEntry[] = names.map((name) => ({
                name,
                relativePath: [directory.relativePath, name].filter(Boolean).join('/'),
                kind: name.includes('.') ? 'FILE' : 'DIRECTORY',
                size: name.includes('.') ? 1474560 : null,
            }));
            return { directory, entries, truncated: false, nextCursor: null };
        },
        startFloppyInspection: async () => job,
        waitForJob: async () => job,
        releaseFloppyInspection: async () => {},
        cancelJob: async () => {},
    } as unknown as ImageTransport;
    const { floppyImportWorkflow: floppy, packageBatchImportWorkflow: packages } = createObjectImports({
        transport,
        jobs: new JobController(transport),
        picker,
        pickerHistory: new PackagePickerHistory(),
        isDesktop: true,
        sessionId: () => 1,
        sourceItems: () => [partition],
        mutationsAvailable: () => true,
        invalidateSession: async () => {},
        refreshSession: async () => {},
        setStatus: () => {},
        otherFormat: async () => {},
    });
    function context(event: MouseEvent, item: DiskTreeItem): void {
        event.preventDefault();
        menu = { item, left: event.clientX, top: event.clientY };
    }
    async function choose(item: DiskTreeItem, action: ImageTreeAction): Promise<void> {
        menu = null;
        if (action === 'import-floppy') await floppy.chooseFiles(item);
        else if (action === 'import-packages') {
            packages.open(item);
            await packages.chooseWorkspace();
            await packages.close();
        }
    }
</script>

<div class="fixture-toolbar">
    <button class="secondary-button" oncontextmenu={(event) => context(event, volume)}>Volume</button>
    <button class="secondary-button" oncontextmenu={(event) => context(event, partition)}>Partition</button>
    <button
        class="secondary-button"
        onclick={() => {
            missing = !missing;
        }}
    >
        {missing ? 'Restore disk folder' : 'Remove disk folder'}
    </button>
</div>
{#if menu}
    {@const item = menu.item}
    <ImageTreeContextMenu
        {item}
        left={menu.left}
        top={menu.top}
        volumeActionsEnabled={false}
        partitionActionsEnabled={false}
        packageImportEnabled={true}
        packageExportEnabled={false}
        volumePackageExportEnabled={false}
        volumeFloppyExportEnabled={false}
        audioExportEnabled={false}
        mediaConversionEnabled={false}
        allocationInspectionEnabled={false}
        onaction={(action) => void choose(item, action)}
        onclose={() => {
            menu = null;
        }}
    />
{/if}
<FloppyImportDialog workflow={floppy} />
<PickerDialogHost
    {transport}
    request={pickerRequest}
    finish={(selection) => picker.finish(selection)}
    manageLocations={() => {}}
/>

<style>
    .fixture-toolbar {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
        padding: 12px;
    }
</style>
