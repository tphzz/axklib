<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { ProgramEditingSnapshot } from '../../../../lib/objectEditing';
    import { ProgramDraft, type DraftAssignment } from './draft.svelte';
    import KeyboardMapping from '../../../object-editor/KeyboardMapping.svelte';
    import { noteName } from '../sample/geometry';
    import { mappingLimits, mappingPatch, mappingZones } from './mappingModel';
    import { mappingEditor } from '../../../program-mapping/controller.svelte';
    import Icon from '../../../../lib/components/Icon.svelte';
    import { userFacingMessage } from '../../../../lib/userFacingMessage';
    let {
        document,
        snapshot,
        assignment,
        disabled,
    }: {
        document: ObjectEditorDocument;
        snapshot: ProgramEditingSnapshot;
        assignment: DraftAssignment;
        disabled: boolean;
    } = $props();
    const draft = $derived(document.draft as ProgramDraft);
    const mapping = mappingEditor();
    const limits = $derived(mappingLimits(draft.values, assignment.id));
    const zones = $derived(mappingZones(snapshot, draft.assignments, draft.values, assignment.id));
</script>

{#if Object.values(limits).every(Number.isFinite)}
    <KeyboardMapping
        {zones}
        {limits}
        formatNote={noteName}
        {disabled}
        onselect={(id) => (document.programAssignmentId = Number(id.split(':')[0]))}
        onchange={(range) => draft.patch(mappingPatch(assignment.id, range))}
        onbegin={() => draft.beginGesture()}
        onend={() => draft.endGesture()}
    >
        {#snippet tools()}
            <button
                class="mapping-open"
                disabled={!mapping?.available}
                title={mapping?.available ? 'Open Mapping Editor' : 'Available in the desktop application'}
                onclick={() => void mapping?.open().catch((error) => (document.status = userFacingMessage(error)))}
            >
                <Icon name="grid" size={14} />Mapping Editor
            </button>
        {/snippet}
    </KeyboardMapping>
    {#if !zones.some((zone) => zone.selected)}<p class="empty-copy">
            No resolved playable range for this assignment.
        </p>{/if}
{/if}

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
    }
</style>
