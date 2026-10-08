<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import RangeBand from '../../../object-editor/RangeBand.svelte';
    import ProgramField from './ProgramField.svelte';
    import { assignmentPages, programField } from './fields';
    let {
        document,
        assignmentId,
        disabled,
    }: { document: ObjectEditorDocument; assignmentId: number; disabled: boolean } = $props();
    const prefix = $derived(`assignments.${assignmentId}.`);
    const low = $derived(Number(document.draft.values[prefix + 'velocity_low']));
    const high = $derived(Number(document.draft.values[prefix + 'velocity_high']));
    const keys = $derived((assignmentPages.range ?? []).filter((key) => key.startsWith('key_')));
    const velocity = $derived((assignmentPages.range ?? []).filter((key) => !key.startsWith('key_')));
</script>

{#snippet controls(keys: string[])}
    {#each keys as key}
        {@const field = programField(prefix + key, document.programFormat!, document.draft.values)}
        {#if field}<ProgramField {document} {field} {disabled} />{/if}
    {/each}
{/snippet}
<div class="range-groups">
    <section aria-label="Key mapping">
        <h3>Key mapping</h3>
        <div class="fields">{@render controls(keys)}</div>
    </section>
    <section aria-label="Velocity range">
        <h3>Velocity range</h3>
        <div class="velocity-range">
            <RangeBand
                label="Velocity"
                lowLabel="Soft"
                highLabel="Hard"
                {low}
                {high}
                disabled={disabled || !Number.isFinite(low) || !Number.isFinite(high)}
                onbegin={() => document.draft.beginGesture()}
                onend={() => document.draft.endGesture()}
                onchange={(low, high) =>
                    document.draft.patch({ [prefix + 'velocity_low']: low, [prefix + 'velocity_high']: high })}
            />
            <div class="fields velocity-fields">{@render controls(velocity)}</div>
        </div>
    </section>
</div>

<style>
    .range-groups {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(min(100%, 380px), 1fr));
        gap: 12px 18px;
    }
    section {
        min-width: 0;
    }
    h3 {
        font-size: 11px;
        font-weight: 600;
        margin: 0 0 8px;
    }
    .fields {
        display: grid;
        gap: 6px 12px;
    }
    .velocity-range {
        container: program-velocity / inline-size;
        display: flex;
        gap: 10px;
        align-items: start;
    }
    .velocity-fields {
        flex: 1;
        min-width: 0;
        grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    .velocity-fields :global(.parameter-field) {
        display: flex;
        flex-direction: column;
        align-items: stretch;
        gap: 2px;
    }
    @container program-velocity (max-width: 340px) {
        .velocity-fields {
            grid-template-columns: minmax(0, 1fr);
        }
    }
</style>
