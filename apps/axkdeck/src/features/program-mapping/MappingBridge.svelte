<script lang="ts">
    import { onMount, untrack } from 'svelte';
    import type { InspectorSelection } from '../../lib/types';
    import type { ObjectEditorDocument, ObjectEditorWorkflow } from '../object-editor/workflow.svelte';
    import type { MappingController } from './controller.svelte';
    import { userFacingMessage } from '../../lib/userFacingMessage';
    let {
        controller,
        editors,
        sessionId,
        selection,
    }: {
        controller: MappingController;
        editors: ObjectEditorWorkflow;
        sessionId: number | null;
        selection: InspectorSelection;
    } = $props();
    let document = $state.raw<ObjectEditorDocument | null>(null);
    const objectId = $derived(selection?.kind === 'program' ? selection.program.objectId : null);
    $effect(() => {
        const id = objectId,
            session = sessionId,
            opened = controller.opened;
        controller.retryVersion;
        let current = true;
        document = null;
        untrack(() => controller.beginSelection());
        if (opened && id && session !== null)
            void untrack(() => editors.load(session, id))
                .then((value) => {
                    if (current) document = value;
                })
                .catch((error) => {
                    if (current) controller.loadFailed(userFacingMessage(error));
                });
        return () => {
            current = false;
        };
    });
    $effect(() => {
        controller.refresh(document && editors.documents.includes(document) ? document : null);
    });
    onMount(() => {
        let disposed = false;
        if ('__TAURI_INTERNALS__' in window)
            void import('./desktop')
                .then(async ({ mappingHostAdapter }) => {
                    if (disposed) return;
                    await controller.connect(mappingHostAdapter);
                    if (disposed) controller.dispose();
                })
                .catch((error) => {
                    if (document) document.status = userFacingMessage(error);
                });
        return () => {
            disposed = true;
            controller.dispose();
        };
    });
</script>
