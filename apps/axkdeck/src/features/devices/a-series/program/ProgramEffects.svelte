<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import GraphPanel from '../../../object-editor/GraphPanel.svelte';
    import EditorAutocomplete from '../../../object-editor/EditorAutocomplete.svelte';
    import ExtendedParameterMarker from '../../../object-editor/ExtendedParameterMarker.svelte';
    import EffectRouting from './EffectRouting.svelte';
    import ProgramField from './ProgramField.svelte';
    import { ProgramDraft } from './draft.svelte';
    import { programField } from './fields';
    let {
        document,
        page,
        slot,
        onselect,
        disabled,
    }: {
        document: ObjectEditorDocument;
        page: string;
        slot: number;
        onselect: (value: number) => void;
        disabled: boolean;
    } = $props();
    const format = $derived(document.programFormat!);
    const values = $derived(document.draft.values);
    const native = $derived(format.model === 'A3000');
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
            options={format.effects.map((type) => ({ value: type.id, label: `${type.printedNumber}: ${type.label}` }))}
            {disabled}
            onchange={changeType}
        />
    </div>
    <div class="fields" class:single={page === 'routing'}>
        {#each ['enabled', 'input_level', 'output_level', 'pan', 'width', 'destination'] as key}
            {@const control = programField(`effects.${slot}.${key}`, format, values)}
            {#if control}<ProgramField {document} field={control} {disabled} />{/if}
        {/each}
        {#if page === 'parameters'}
            {#each effect?.parameters.filter((item) => item.editable) ?? [] as parameter}
                <ProgramField
                    {document}
                    field={{
                        key: `effects.${slot}.words.${parameter.index}`,
                        label: parameter.label,
                        min: parameter.min,
                        max: parameter.max,
                    }}
                    {disabled}
                />
            {/each}
        {/if}
    </div>
{/snippet}
{#if page === 'routing'}
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
{:else}
    <div class="effect-tabs" role="group" aria-label="Effect slots">
        {#each Array.from({ length: native ? 3 : 6 }, (_, i) => i + 1) as value}
            <button
                class="editor-control"
                aria-label={`Select Ef${value}`}
                aria-pressed={value === slot}
                onclick={() => onselect(value)}
            >
                Ef{value}{#if value > 3}<ExtendedParameterMarker a5000Only decorative />{/if}
            </button>
        {/each}
    </div>
    {@render common()}
{/if}

<style>
    .fields {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(min(100%, 330px), 1fr));
        gap: 6px 18px;
        margin: 8px 0;
    }
    .fields.single {
        grid-template-columns: minmax(0, 1fr);
    }
    .connections {
        display: grid;
        gap: 6px;
    }
    .effect-heading {
        display: grid;
        grid-template-columns: auto minmax(0, 1fr);
        align-items: center;
        gap: 8px;
        margin: 4px 0 10px;
    }
    strong {
        font-size: 11px;
        font-weight: 600;
    }
    .effect-tabs {
        display: flex;
        gap: 4px;
        flex-wrap: wrap;
        margin-bottom: 8px;
    }
    .effect-tabs button {
        width: auto;
        min-width: 44px;
    }
    .effect-tabs button[aria-pressed='true'] {
        border-color: var(--color-accent);
        background: var(--color-panel-raised);
    }
</style>
