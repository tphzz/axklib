<script lang="ts">
    import FilesEditDialog from '../features/files/FilesEditDialog.svelte';
    import FilesImportDialog from '../features/files/FilesImportDialog.svelte';
    import { FilesEditWorkflow } from '../features/files/editWorkflow.svelte';
    import { FilesImportWorkflow } from '../features/files/importWorkflow.svelte';
    import { filesystemEntry, writableFilesRoot } from '../lib/testing/filesystem';
    import { serverFileLocation } from '../lib/storageLocations';
    const params = new URLSearchParams(location.search);
    const capabilities = {
        ...writableFilesRoot,
        namePolicy: 'FAT_8_3_UPPERCASE' as const,
        maximumNameBytes: 12,
        namePattern: '^[A-Z0-9]{1,8}(\\.[A-Z0-9]{1,3})?$',
        nameHint: 'Use 1-8 ASCII characters plus an optional 1-3 character extension.',
    };
    const completed = { jobId: 1, kind: 'test', status: 'completed' as const };
    const driver = {
        execute: async () => completed,
        observe: async () => completed,
        cancel: async () => {},
        refresh: async () => {},
    };
    const edit = new FilesEditWorkflow();
    const imports = new FilesImportWorkflow();
    if (params.get('kind') === 'import') {
        imports.open(
            1,
            filesystemEntry(),
            capabilities,
            {
                supportsClientUploads: false,
                chooseFiles: async () => [],
                chooseDirectory: async () => null,
                upload: async () => [],
                release: async () => {},
                inspectInputs: async () => completed,
                inspectDestination: async () => completed,
                observe: async () => completed,
                cancel: async () => {},
            },
            driver,
        );
        imports.phase = 'dirty';
        imports.rows = ['OK.BIN', 'TOOLONGNAME.BIN', 'OTHER.BIN'].map((name, index) => ({
            directory: false,
            name,
            parent: null,
            conflict: 'SKIP',
            source: serverFileLocation({ rootId: 'source', relativePath: name }),
            snapshot: { revision: 'rev', sizeBytes: 4, sha256: 'a'.repeat(64) },
            decision:
                index === 2
                    ? {
                          relativePath: [name],
                          directory: false,
                          sizeBytes: 4,
                          conflict: 'SKIP',
                          action: 'CONFLICT',
                          issue: 'A directory already uses this name.',
                          existingSizeBytes: null,
                      }
                    : null,
        }));
    } else edit.open({ kind: 'create', revision: 1, entries: [filesystemEntry()], capabilities }, driver);
</script>

<main style:zoom={Number(params.get('scale') ?? 1)}>
    {#if imports.target}<FilesImportDialog workflow={imports} />{/if}
    {#if edit.review}<FilesEditDialog workflow={edit} />{/if}
</main>
