<script lang="ts">
    import { onMount } from 'svelte';
    import FilesView from '../features/files/FilesView.svelte';
    import { FilesController } from '../features/files/controller.svelte';
    import type { FilesystemAccess, FilesystemMutationDriver } from '../lib/filesystem';
    import type { FilesystemImportActions } from '../lib/filesystemImport';
    import { filesystemEntry, writableFilesRoot } from '../lib/testing/filesystem';
    import { serverFileLocation } from '../lib/storageLocations';

    const root = filesystemEntry({
        id: 'root',
        name: 'EX5 MO',
        path: '',
        kind: 'root',
        parentId: null,
        ancestorIds: [],
    });
    const folder = filesystemEntry({ id: 'folder', name: 'SAMPLES', path: '/SAMPLES' });
    const closed = filesystemEntry({ id: 'closed', name: 'CLOSED', path: '/CLOSED' });
    let entries = [
        root,
        folder,
        closed,
        ...Array.from({ length: 80 }, (_, index) =>
            filesystemEntry({
                id: `file-${index}`,
                name: `F${String(index).padStart(3, '0')}.BIN`,
                kind: 'file',
                path: `/SAMPLES/F${String(index).padStart(3, '0')}.BIN`,
                parentId: folder.id,
                ancestorIds: ['root', folder.id],
            }),
        ),
    ];
    let revision = 1;
    let writes = $state(0);
    let status = $state('');
    const capabilities = {
        ...writableFilesRoot,
        namePolicy: 'FAT_8_3_UPPERCASE' as const,
        maximumNameBytes: 12,
        namePattern: '^[A-Z0-9]{1,8}(\\.[A-Z0-9]{1,3})?$',
        nameHint: 'Use an 8.3 filename.',
    };
    const access: FilesystemAccess = {
        inspect: async (query = {}) => {
            // Leave enough time for DOM layout between root and child responses.
            await new Promise((resolve) => setTimeout(resolve, 20));
            const matches = query.entryId
                ? entries.filter((entry) => entry.id === query.entryId)
                : query.parentId
                  ? entries.filter((entry) => entry.parentId === query.parentId)
                  : query.rootId
                    ? entries.filter((entry) => entry.name.includes(query.query ?? ''))
                    : [root];
            return {
                revision,
                available: true,
                filesystemName: 'FAT16',
                deviceView: null,
                items: matches.slice(query.offset ?? 0, (query.offset ?? 0) + (query.limit ?? 200)),
                totalCount: matches.length,
                rootCapabilities: [capabilities],
            };
        },
    };
    let controller = $state(new FilesController(access));
    const completed = { jobId: 1, kind: 'test', status: 'completed' as const };
    const driver: FilesystemMutationDriver = {
        execute: async (_revision, edits) => {
            writes++;
            for (const edit of edits) {
                if (edit.kind !== 'PUT_FILE') throw new Error('Unexpected edit');
                const name = edit.relativePath.join('/');
                entries = [
                    ...entries,
                    filesystemEntry({
                        id: 'imported',
                        name,
                        kind: 'file',
                        parentId: edit.parentEntryId,
                        ancestorIds: ['root', folder.id],
                        path: `/SAMPLES/${name}`,
                    }),
                ];
            }
            revision++;
            return { ...completed, result: { imageId: 'image', revision, warnings: [] } };
        },
        observe: async () => completed,
        cancel: async () => {},
        refresh: async () => {
            const context = controller.capture();
            controller.dispose();
            controller = new FilesController(access);
            await controller.initialize(context);
        },
    };
    const source = serverFileLocation({ rootId: 'host', relativePath: 'tone.bin' });
    const imports: FilesystemImportActions = {
        supportsClientUploads: true,
        chooseFiles: async () => [source],
        chooseDirectory: async () => null,
        upload: async () => [source],
        release: async () => {},
        inspectInputs: async () => ({
            ...completed,
            result: {
                inputs: [
                    {
                        source: { fileRef: source.reference },
                        snapshot: { revision: 'rev', sizeBytes: 4, sha256: 'a'.repeat(64) },
                    },
                ],
            },
        }),
        inspectDestination: async (revision, parentEntryId, entries) => ({
            ...completed,
            result: {
                imageId: 'image',
                revision,
                parentEntryId,
                conflictCount: 0,
                entries: entries.map((entry) => ({
                    ...entry,
                    action: 'CREATE_FILE',
                    issue: '',
                    existingSizeBytes: null,
                })),
            },
        }),
        observe: async () => completed,
        cancel: async () => {},
    };
    onMount(() => {
        void controller.initialize();
    });
</script>

<main data-navigation-workspace>
    <FilesView {controller} {driver} {imports} setStatus={(message) => (status = message)} />
    <output data-testid="status">{status}</output>
    <output data-testid="writes">{writes}</output>
</main>

<style>
    main {
        height: 90vh;
        display: flex;
        flex-direction: column;
    }
</style>
