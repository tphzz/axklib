<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { ProgramEditingSnapshot } from '../../../../lib/objectEditing';
    import type { DraftAssignment } from './draft.svelte';
    import EnvelopeGraph from '../sample/EnvelopeGraph.svelte';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import { previewSamples, programEnvelope } from './visualization';
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
    let previewId = $state('');
    const samples = $derived(previewSamples(snapshot, assignment));
    const sample = $derived(samples.find((item) => item.objectId === previewId) ?? samples[0]);
    const curve = $derived(sample ? programEnvelope(sample, document.draft.values, assignment.id) : null);
</script>

{#if assignment.kind === 'SBAC'}
    <div class="preview-member">
        <span>Preview sample</span><EditorChoice
            label="Envelope preview sample"
            value={samples.indexOf(sample!)}
            options={samples.map((item, index) => ({ value: index, label: item.name }))}
            disabled={!samples.length}
            segmented={false}
            onchange={(index) => (previewId = samples[index]!.objectId)}
        />
    </div>
{/if}
{#if curve && sample}
    <div class="envelope-graph">
        {#key `${assignment.id}:${sample.objectId}`}
            <EnvelopeGraph
                draft={document.draft}
                kind="aeg"
                {disabled}
                blocked={['aeg.sustain_level']}
                values={curve.effective}
                baseValues={sample.values}
                rateOffsets={curve.bindings}
                onselect={() => {}}
            >
                {#snippet title()}<span>Amplitude envelope</span>{/snippet}
            </EnvelopeGraph>
        {/key}
        <span class="legend"><i></i>Base <i class="effective"></i>Easy Edit</span>
    </div>
{:else}<p class="empty-copy">Envelope preview unavailable for this assignment.</p>{/if}

<style>
    .envelope-graph {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 230px;
        min-width: 0;
    }
    .preview-member {
        display: grid;
        grid-template-columns: 90px minmax(0, 250px);
        align-items: center;
        gap: 8px;
        margin: 6px 0;
        color: var(--color-text-muted);
        font-size: 10px;
    }
    .legend {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        flex: none;
        height: 18px;
        justify-content: flex-end;
        padding-right: 8px;
        color: var(--color-text-muted);
        font-size: 10px;
    }
    i {
        width: 16px;
        border-top: 1px dashed var(--color-text-muted);
    }
    i.effective {
        border-top: 2px solid var(--editor-loop);
    }
</style>
