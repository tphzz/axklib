<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import GraphPanel from '../../../object-editor/GraphPanel.svelte';
    import EditorAutocomplete from '../../../object-editor/EditorAutocomplete.svelte';
    import ExtendedParameterMarker from '../../../object-editor/ExtendedParameterMarker.svelte';
    import EffectRouting from './EffectRouting.svelte';
    import ProgramField from './ProgramField.svelte';
    import { ProgramDraft } from './draft.svelte';
    import { programField } from './fields';
    import { effectParameterExtended } from './effectCapabilities';
    let {
        document,
        slot,
        onselect,
        disabled,
    }: {
        document: ObjectEditorDocument;
        slot: number;
        onselect: (value: number) => void;
        disabled: boolean;
    } = $props();
    const format = $derived(document.programFormat!);
    const values = $derived(document.draft.values);
    const native = $derived(format.model === 'A3000');
    const earlier = $derived(document.programCatalog?.formats.find((format) => format.model === 'A3000'));
    const effect = $derived(format.effects.find((item) => item.id === values[`effects.${slot}.type`]));
    function changeType(value: number) {
        const type = format.effects.find((item) => item.id === value);
        if (!disabled && type && document.draft instanceof ProgramDraft)
            document.draft.changeEffectType(slot, value, type.resetWords);
    }
</script>

{#snippet common()}
    <div class="effect-heading">
        <strong
            >Ef{slot}{#if slot > 3}<ExtendedParameterMarker a5000Only />{/if}</strong
        >
        <EditorAutocomplete
            label="Effect type"
            value={Number(values[`effects.${slot}.type`])}
            options={format.effects.map((type) => ({
                value: type.id,
                label: `${type.printedNumber}: ${type.label}`,
                extended: !native && !!earlier && !earlier.effects.some((effect) => effect.id === type.id),
            }))}
            {disabled}
            onchange={changeType}
        />
    </div>
    <div class="fields">
        {#each ['enabled', 'input_level', 'output_level', 'pan', 'width', 'destination'] as key}
            {@const control = programField(`effects.${slot}.${key}`, format, values)}
            {#if control}<ProgramField
                    {document}
                    field={{
                        ...control,
                        a5000Only: false,
                        nativeKey: control.key.replace(/^effects\.\d+\./, 'effects.1.'),
                    }}
                    {disabled}
                />{/if}
        {/each}
    </div>
    {#if effect?.parameters.some((item) => item.editable)}
        <hr />
        <h3>Effect parameters</h3>
        <div class="fields">
            {#each effect?.parameters.filter((item) => item.editable) ?? [] as parameter}
                <ProgramField
                    {document}
                    field={{
                        key: `effects.${slot}.words.${parameter.index}`,
                        label: parameter.label,
                        min: parameter.min,
                        max: parameter.max,
                        extended: effectParameterExtended(format, earlier, effect!.id, parameter.index),
                    }}
                    {disabled}
                />
            {/each}
        </div>
    {/if}
{/snippet}
<GraphPanel
    label="Effect routing"
    layoutKey="program-routing"
    graphMinimum={420}
    controlsMinimum={300}
    stackBelow={740}
>
    {#snippet graph()}
        <div class="connections">
            {#each native ? ['effect_connections.1'] : ['effect_connections.1', 'effect_connections.2'] as key}
                {@const control = programField(key, format, values)}
                {#if control}<ProgramField {document} field={control} {disabled} />{/if}
            {/each}
        </div>
        <EffectRouting {document} selected={slot} {onselect} />
    {/snippet}
    {#snippet controls()}{@render common()}{/snippet}
</GraphPanel>

<style>
    .fields {
        display: grid;
        grid-template-columns: minmax(0, 1fr);
        gap: 6px 18px;
        margin: 8px 0;
    }
    .connections {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(min(100%, 205px), 1fr));
        gap: 6px;
    }
    .effect-heading {
        display: grid;
        grid-template-columns: auto minmax(0, 1fr);
        align-items: center;
        gap: 8px;
        margin: 4px 0 10px;
        position: sticky;
        top: 0;
        z-index: 1;
        background: var(--color-panel);
        padding-block: 4px;
    }
    strong,
    h3 {
        font-size: 11px;
        font-weight: 600;
    }
    hr {
        border: 0;
        border-top: 1px solid var(--color-border);
        margin: 12px 0;
    }
</style>
