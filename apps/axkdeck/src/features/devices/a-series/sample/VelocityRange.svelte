<script lang="ts">
    import RangeBand from '../../../object-editor/RangeBand.svelte';
    import ParameterField from './ParameterField.svelte';
    import type { SampleField } from './fields';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    let { document, fields, disabled }: { document: ObjectEditorDocument; fields: SampleField[]; disabled: boolean } =
        $props();
    const blocked = (key: string) => disabled || document.detail!.editing!.blockedParameters.includes(key);
    const unavailable = $derived(
        ['velocity_low', 'velocity_high'].some((key) => !Number.isFinite(document.draft.values[key]) || blocked(key)),
    );
</script>

<div class="velocity-range">
    <RangeBand
        label="Velocity"
        lowLabel="Soft"
        highLabel="Hard"
        low={Number(document.draft.values.velocity_low ?? 0)}
        high={Number(document.draft.values.velocity_high ?? 127)}
        disabled={unavailable}
        onbegin={() => document.draft.beginGesture()}
        onend={() => document.draft.endGesture()}
        onchange={(low, high) => document.draft.patch({ velocity_low: low, velocity_high: high })}
    />
    <div class="velocity-fields">
        {#each fields as field}
            <ParameterField
                {field}
                draft={document.draft}
                unavailableReason={document.detail?.editing?.unavailableParameters[field.key]?.message}
                disabled={blocked(field.key)}
                slider={field.key.includes('xfade')}
                oninvalid={(message) => (document.inputErrors = { ...document.inputErrors, [field.key]: message })}
            />
        {/each}
    </div>
</div>

<style>
    .velocity-range {
        display: flex;
        align-items: start;
        gap: 10px;
        min-width: 0;
    }
    .velocity-fields {
        flex: 1;
        min-width: 0;
        display: grid;
        grid-template-columns: repeat(2, minmax(0, 1fr));
        gap: 8px 12px;
    }
    .velocity-fields :global(.parameter-field) {
        display: flex;
        flex-direction: column;
        align-items: stretch;
        gap: 2px;
    }
    .velocity-fields :global(.editor-value) {
        flex: 1;
    }
    .velocity-fields :global(.editor-number:has(.editor-slider) .editor-value) {
        flex: 0 0 45px;
    }
    .velocity-fields :global(.editor-number) {
        gap: 5px;
    }
</style>
