<script lang="ts">
    import Icon from '../../lib/components/Icon.svelte';
    import { untrack } from 'svelte';
    import { userFacingMessage } from '../../lib/userFacingMessage';
    import type { ObjectEditorDocument } from '../object-editor/workflow.svelte';
    import type { MappingRole } from './protocol';
    import { mappingEditor } from './controller.svelte';
    let {
        role,
        document,
        label = 'Mapping Editor',
    }: { role: MappingRole; document: ObjectEditorDocument; label?: string } = $props();
    const controller = mappingEditor(untrack(() => role));
</script>

<button
    class="mapping-open"
    disabled={!controller?.available}
    title={controller?.available ? `Open ${label}` : 'Available in the desktop application'}
    onclick={() => void controller?.open().catch((error) => (document.status = userFacingMessage(error)))}
>
    <Icon name="grid" size={14} />{label}
</button>

<style>
    .mapping-open {
        display: inline-flex;
        align-items: center;
        gap: 5px;
        height: 26px;
        padding: 0 7px;
        border: 1px solid var(--color-border);
        border-radius: 3px;
        font-size: 11px;
        white-space: nowrap;
    }
</style>
