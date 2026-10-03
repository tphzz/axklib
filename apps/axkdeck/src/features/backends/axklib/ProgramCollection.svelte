<script lang="ts">
    import type { ComponentProps } from 'svelte';
    import ProgramWorkspace from '../../../lib/components/ProgramWorkspace.svelte';
    import type { WorkspaceProps } from './workspaceProps';
    import { objectEditors } from '../../object-editor/context';
    let {
        sessionId,
        catalog,
        ...props
    }: Omit<
        ComponentProps<typeof ProgramWorkspace>,
        'contexts' | 'contextsLoading' | 'contextsError' | 'onconvertprogram'
    > &
        Pick<WorkspaceProps, 'sessionId' | 'catalog'> = $props();
    const editors = objectEditors();
</script>

<ProgramWorkspace
    {...props}
    contexts={catalog.systemProgramContexts}
    contextsLoading={catalog.systemProgramContextsLoading}
    contextsError={catalog.systemProgramContextsError}
    onconvertprogram={editors && sessionId !== null
        ? (program) => void editors.openConversion(sessionId!, program.objectId)
        : undefined}
/>
