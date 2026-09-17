<script lang="ts">
    import { onDestroy, untrack, tick } from 'svelte';
    import FilesView from '../../files/FilesView.svelte';
    import Su700ImportDialog from './Su700ImportDialog.svelte';
    import { Su700Workflow } from './su700Workflow.svelte';
    import { openASeriesFloppy } from './floppyFilesImport';
    import type { AxklibFilesystemImports } from './su700Actions';
    import type { FilesController } from '../../files/controller.svelte';
    import type { FilesystemEntry, FilesystemMutationDriver } from '../../../lib/filesystem';
    import type { FilesystemExportActions } from '../../../lib/filesystemExport';
    let {
        controller,
        sessionId,
        driver,
        imports,
        exports,
        onexport,
        exportBlocked = false,
        setStatus = () => undefined,
    }: {
        controller: FilesController;
        sessionId: number;
        driver?: FilesystemMutationDriver;
        imports?: AxklibFilesystemImports;
        exports?: FilesystemExportActions;
        onexport?: (entries: FilesystemEntry[]) => void;
        exportBlocked?: boolean;
        setStatus?: (message: string) => void;
    } = $props();
    let view: FilesView;
    // This wrapper is keyed by image session; keep an in-flight review bound to that session.
    const workflow = untrack(() =>
        imports && driver
            ? new Su700Workflow(
                  () => controller,
                  imports,
                  sessionId,
                  async () => {
                      await driver!.refresh();
                      await tick();
                      await controller.initialize();
                  },
                  (message) => setStatus(message),
              )
            : null,
    );
    const imageImport = $derived(
        imports?.floppy?.available
            ? {
                  label: 'Import floppy...',
                  enabled: true,
                  busy: !!imports.floppy.request,
                  open: (
                      entries?: import('../../../lib/filesystemImport').ClientFilesystemImportEntry[],
                      target?: FilesystemEntry,
                  ) => openASeriesFloppy(imports!.floppy!, controller, entries, target),
              }
            : workflow
              ? {
                    label: 'Import SU700 floppy...',
                    enabled: workflow.enabled,
                    busy: workflow.opened,
                    open: workflow.open.bind(workflow),
                }
              : undefined,
    );
    export function isBusy() {
        return !!workflow?.opened || !!imports?.floppy?.request || (view?.isBusy() ?? false);
    }
    export function importRoot(root: FilesystemEntry) {
        view?.importRoot(root);
    }
    export function importCommands(root: FilesystemEntry) {
        if (imports?.floppy?.available && !imports.floppy.request && !exportBlocked)
            return [
                {
                    label: 'Import floppy...',
                    action: async () => {
                        await openASeriesFloppy(imports!.floppy!, controller, undefined, root);
                    },
                },
            ];
        if (
            !workflow?.enabled ||
            workflow.opened ||
            exportBlocked ||
            !controller.capabilitiesFor(root.id)?.supportedImports.includes('SU700_FLOPPY')
        )
            return [];
        return [
            {
                label: 'Import SU700 floppy...',
                action: async () => {
                    await controller.chooseRoot(root.id);
                    await workflow.open();
                },
            },
        ];
    }
    onDestroy(() => workflow?.dispose());
</script>

<FilesView
    bind:this={view}
    {controller}
    {driver}
    {imports}
    {exports}
    {onexport}
    {exportBlocked}
    {imageImport}
    {setStatus}
/>
{#if workflow}<Su700ImportDialog {workflow} />{/if}
