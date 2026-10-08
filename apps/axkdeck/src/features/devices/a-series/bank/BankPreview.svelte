<script lang="ts">
    import { sampleSnapshot } from '../../../../lib/objectEditing';
    import { untrack, tick } from 'svelte';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import { objectEditors } from '../../../object-editor/context';
    import { editorAudio } from '../../../object-editor/audioContext';
    import { sampleView } from '../sample/view.svelte';
    import { BankDraft } from './draft.svelte';
    import Icon from '../../../../lib/components/Icon.svelte';
    import { userFacingMessage } from '../../../../lib/userFacingMessage';
    import { mappingMembers } from './mappingMembers';
    import { loadMappingMember } from './mappingLoad';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    let { document, onready }: { document: ObjectEditorDocument; onready: () => void } = $props();
    const editors = objectEditors();
    const audio = editorAudio();
    const members = $derived(sampleSnapshot(document.detail)!.bankOverrides!.members);
    const confirmed = $derived(mappingMembers(document.detail!));
    const isConfirmed = (id: string | null) => confirmed.some((member) => member.id === id);
    const index = $derived(members.findIndex((member) => member.objectId === document.previewMemberId));
    const options = $derived([
        ...(index < 0
            ? [{ value: -1, label: members.length ? 'Unresolved member' : 'No samples', disabled: true }]
            : []),
        ...members.map((member, value) => {
            const resolved = isConfirmed(member.objectId);
            return { value, label: `${member.name}${resolved ? '' : ' (unresolved)'}`, disabled: !resolved };
        }),
    ]);
    const previous = $derived(members.slice(0, Math.max(0, index)).findLast((member) => isConfirmed(member.objectId)));
    const next = $derived(members.slice(index + 1).find((member) => isConfirmed(member.objectId)));
    function select(id: string | null) {
        document.previewMemberId = id;
    }
    $effect(() => {
        const member = editors?.find(document.sessionId, document.previewMemberId ?? '');
        if (
            member &&
            isConfirmed(member.detail!.object.id) &&
            member.detail?.image.revision === document.detail?.image.revision &&
            document.draft instanceof BankDraft
        )
            document.draft.member = {
                ...member.draft.values,
                ...(sampleSnapshot(member.detail)?.sampleFormat.format === 'A3000_188' ? { sample_eq_type: 0 } : {}),
            };
    });
    $effect(() => {
        const revision = document.detail!.image.revision;
        const id = document.previewMemberId;
        if (id === undefined) {
            document.previewMemberId = confirmed[0]?.id ?? null;
            return;
        }
        let current = true;
        untrack(() => {
            editors?.stop();
            sampleView(document).release();
            document.previewDetail = null;
            if (document.draft instanceof BankDraft) document.draft.member = {};
            const resolved = id && isConfirmed(id);
            document.previewStatus = !members.length
                ? 'This bank has no samples'
                : !resolved
                  ? 'The preview sample is unresolved or no longer in this bank'
                  : 'Loading preview sample';
            if (id && resolved && editors)
                void loadMappingMember(editors, document, id)
                    .then(async (member) => {
                        if (!current) return;
                        const detail = member.detail!;
                        if (detail.image.revision !== revision || detail.editing?.profile !== 'a-series/sample')
                            throw new Error('Preview sample is unavailable at this image revision');
                        document.previewDetail = detail;
                        if (document.draft instanceof BankDraft)
                            document.draft.member = {
                                ...member.draft.values,
                                ...(detail.editing.sampleFormat.format === 'A3000_188' ? { sample_eq_type: 0 } : {}),
                            };
                        document.previewStatus = '';
                        sampleView(document).note = Number(document.draft.values.root_key ?? 60);
                        await tick();
                        if (current && audio?.audition.autoplay) onready();
                    })
                    .catch((error) => {
                        if (current) document.previewStatus = userFacingMessage(error);
                    });
        });
        return () => {
            current = false;
        };
    });
</script>

<div
    class="bank-preview"
    title={document.previewStatus || 'Graphs and audition use this sample with the bank overrides'}
>
    <button
        class="editor-icon"
        aria-label="Previous preview sample"
        disabled={!previous}
        onclick={() => select(previous!.objectId)}
        ><span class="previous"><Icon name="chevron" size={13} /></span></button
    >
    <div class="preview-choice">
        <EditorChoice
            label="Preview sample"
            value={index}
            {options}
            segmented={false}
            disabled={!members.length}
            onchange={(value) => {
                const member = members[value];
                if (member?.objectId && isConfirmed(member.objectId)) select(member.objectId);
            }}
        />
    </div>
    <button class="editor-icon" aria-label="Next preview sample" disabled={!next} onclick={() => select(next!.objectId)}
        ><Icon name="chevron" size={13} /></button
    >
</div>

<style>
    .bank-preview {
        display: flex;
        align-items: center;
        gap: 2px;
        flex: 0 0 auto;
    }
    .preview-choice {
        width: 144px;
        flex: 0 0 144px;
        min-width: 0;
    }
    .previous {
        display: flex;
        transform: rotate(180deg);
    }
</style>
