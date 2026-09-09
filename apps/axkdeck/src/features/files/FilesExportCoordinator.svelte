<script lang="ts">
    import { onDestroy } from 'svelte';
    import type { FilesystemEntry } from '../../lib/filesystem';
    import type { FilesystemExportActions } from '../../lib/filesystemExport';
    import { FilesExportWorkflow } from './exportWorkflow.svelte';
    import FilesExportDialog from './FilesExportDialog.svelte';
    let { driver, setStatus }: { driver: FilesystemExportActions; setStatus: (message: string) => void } = $props();
    const workflow = new FilesExportWorkflow((message) => setStatus(message));
    export function open(revision: number, entries: FilesystemEntry[]): void {
        void workflow.open(revision, entries, driver);
    }
    export function isOpen(): boolean {
        return !!workflow.review;
    }
    onDestroy(() => workflow.dispose());
</script>

<FilesExportDialog {workflow} />
