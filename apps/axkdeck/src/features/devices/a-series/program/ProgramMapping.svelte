<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { ProgramEditingSnapshot } from '../../../../lib/objectEditing';
    import { ProgramDraft, type DraftAssignment } from './draft.svelte';
    import KeyboardMapping from '../../../object-editor/KeyboardMapping.svelte';
    import { noteName } from '../sample/geometry';
    import { mappingLimits, mappingPatch, mappingZones } from './mappingModel';
    import MappingOpen from '../../../program-mapping/MappingOpen.svelte';
    import { mappingKeyboard } from '../../../program-mapping/keyboardAudition.svelte';
    import { objectEditors } from '../../../object-editor/context';
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
    const editors = objectEditors();
    const keyboard = mappingKeyboard(
        () => document,
        () => 'program',
    );
    const limits = $derived(mappingLimits(draft.values, assignment.id));
    const zones = $derived(
        mappingZones(snapshot, draft.assignments, draft.values, assignment.id, (id) => {
            const sample = editors?.find(document.sessionId, id);
            return sample?.detail?.image.revision === document.detail?.image.revision ? sample : undefined;
        }),
    );
</script>

{#if Object.values(limits).every(Number.isFinite)}
    <KeyboardMapping
        {zones}
        velocity={keyboard.state.velocity}
        onvelocity={(value) => {
            keyboard.release();
            keyboard.state.velocity = value;
        }}
        onpress={keyboard.press}
        onrelease={keyboard.release}
        {limits}
        formatNote={noteName}
        {disabled}
        rangeLabel="Program limits"
        onselect={(id) => (document.programAssignmentId = Number(id.split(':')[0]))}
        onchange={(range, boundaries) => draft.patch(mappingPatch(assignment.id, range, boundaries))}
        onbegin={() => draft.beginGesture()}
        onend={(cancelled) => (cancelled ? draft.cancelGesture() : draft.endGesture())}
    >
        {#snippet tools()}
            <MappingOpen role="program" {document} />
        {/snippet}
    </KeyboardMapping>
    {#if !zones.some((zone) => zone.selected)}<p class="empty-copy">
            No resolved playable range for this assignment.
        </p>{/if}
{/if}
