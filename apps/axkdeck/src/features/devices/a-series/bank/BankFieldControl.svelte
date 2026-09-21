<script lang="ts">
    import type { SampleField } from '../sample/fields';
    import { BankDraft } from './draft.svelte';
    import EditorNumber from '../../../object-editor/EditorNumber.svelte';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import EditorAutocomplete from '../../../object-editor/EditorAutocomplete.svelte';
    import Icon from '../../../../lib/components/Icon.svelte';
    let {
        field,
        draft,
        disabled,
        oninvalid,
        slider = true,
    }: {
        field: SampleField;
        draft: BankDraft;
        disabled: boolean;
        slider?: boolean;
        oninvalid: (message: string) => void;
    } = $props();
    const unit = $derived(draft.unit(field.key));
    const active = $derived(draft.isOverridden(field.key));
    const value = $derived(draft.values[field.key]);
    const special = $derived(field.special?.some((option) => option.value === value));
    const ordinary = $derived(
        Math.max(
            field.min,
            Math.min(field.max, Number(special ? (draft.baselineValue(field.key) ?? field.min) : (value ?? field.min))),
        ),
    );
    const distanceInactive = $derived(
        field.key === 'filter_cutoff_distance' &&
            draft.isOverridden('filter_type') &&
            (Number(draft.values.filter_type) < 10 || Number(draft.values.filter_type) > 17),
    );
    const locked = $derived(disabled || distanceInactive);
    const help = $derived(
        `${active ? 'Bank override' : "Uses each sample's own value"}. ${unit && unit.keys.length > 1 ? (unit.id === 83 ? 'All six controller assignments share one override. ' : `This hardware group shares ${unit.keys.length} parameter overrides. `) : ''}Editing activates the group using the preview sample's values. Reset restores sample values.`,
    );
</script>

{#if !unit}
    <span class="editor-meta" title="This parameter cannot be overridden by a Sample Bank.">(---)</span>
{:else}
    <div class="bank-control" title={help}>
        {#if field.boolean && !field.options}
            <div class="bank-switch" role="group" aria-label={field.label}>
                {#each [{ text: '---', selected: !active }, { text: 'Off', selected: active && value === false }, { text: 'On', selected: active && value === true }] as choice, index}
                    <button
                        class="editor-control"
                        aria-pressed={choice.selected}
                        {disabled}
                        title={index === 0 ? 'Use sample value' : choice.text}
                        onclick={() =>
                            index === 0 ? draft.clearOverride(field.key) : draft.set(field.key, index === 2)}
                        >{choice.text}</button
                    >
                {/each}
            </div>
        {:else if field.options && (field.key === 'midi_receive_channel' || field.key.startsWith('controls.'))}
            <EditorAutocomplete
                label={field.label}
                value={active ? Number(value) : -9999}
                options={[{ value: -9999, label: '---' }, ...field.options]}
                disabled={locked}
                onchange={(next) => (next === -9999 ? draft.clearOverride(field.key) : draft.set(field.key, next))}
            />
        {:else if field.options}
            <EditorChoice
                label={field.label}
                value={active ? Number(value) : -9999}
                segmented={false}
                options={[{ value: -9999, label: '---' }, ...field.options]}
                disabled={locked}
                onchange={(next) =>
                    next === -9999
                        ? draft.clearOverride(field.key)
                        : draft.set(field.key, field.boolean ? Boolean(next) : next)}
            />
        {:else}
            <div
                class="bank-number"
                class:with-mode={field.special}
                title={distanceInactive
                    ? 'Cutoff Distance requires a dual-filter bank override or inherited Filter Type.'
                    : help}
            >
                {#if field.special}
                    <EditorChoice
                        label={`${field.label} mode`}
                        value={!active ? -9999 : special ? Number(value) : -999}
                        options={[
                            { value: -9999, label: '---' },
                            { value: -999, label: field.note ? 'Key' : 'Amount' },
                            ...field.special,
                        ]}
                        segmented={false}
                        disabled={locked}
                        onchange={(next) =>
                            next === -9999
                                ? draft.clearOverride(field.key)
                                : draft.set(field.key, next === -999 ? ordinary : next)}
                    />
                {/if}
                <EditorNumber
                    label={field.label}
                    value={ordinary}
                    min={field.min}
                    max={field.max}
                    scale={field.scale ?? 1}
                    unit={active ? (field.unit ?? '') : ''}
                    {slider}
                    disabled={locked || (active && special)}
                    inherited={!active}
                    onchange={(next) => draft.set(field.key, next)}
                    onbegin={() => draft.beginGesture()}
                    onend={() => draft.endGesture()}
                    {oninvalid}
                />
            </div>
        {/if}
        <button
            class="editor-icon"
            aria-label={`Use sample values for ${field.label}`}
            title={`Use sample values. ${help}`}
            disabled={disabled || !active}
            onclick={() => draft.clearOverride(field.key)}><Icon name="undo" size={13} /></button
        >
    </div>
{/if}

<style>
    .bank-control {
        display: flex;
        align-items: center;
        min-width: 0;
        gap: 4px;
    }
    .bank-control > :global(:first-child) {
        flex: 1;
        min-width: 0;
    }
    .bank-control :global(.inherited .editor-slider) {
        opacity: 0.45;
    }
    .bank-switch {
        display: flex;
    }
    .bank-switch button {
        flex: 1;
        border-radius: 0;
    }
    .bank-switch button[aria-pressed='true'] {
        background: var(--color-selection);
        border-color: var(--color-accent);
    }
    .bank-number {
        min-width: 0;
    }
    .bank-number.with-mode {
        display: grid;
        grid-template-columns: 80px minmax(0, 1fr);
        gap: 5px;
    }
</style>
