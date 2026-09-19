<script lang="ts">
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import EditorNumber from '../../../object-editor/EditorNumber.svelte';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import EditorAutocomplete from '../../../object-editor/EditorAutocomplete.svelte';
    import type { SampleField } from './fields';
    import type { EditorDraft } from '../../../object-editor/draft.svelte';
    import { noteName } from './geometry';
    import { eqFrequencyLabel } from './eqModel';
    import { measureWidth } from '../../../object-editor/measureWidth';
    let {
        field,
        draft,
        disabled = false,
        slider = true,
        readOnlyText,
        blockedReason = '',
        unavailableReason = '',
        oninvalid = () => {},
    }: {
        field: SampleField;
        draft: EditorDraft;
        disabled?: boolean;
        slider?: boolean;
        readOnlyText?: string;
        blockedReason?: string;
        unavailableReason?: string;
        oninvalid?: (message: string) => void;
    } = $props();
    const value = $derived(draft.values[field.key]);
    const unavailable = $derived(value === undefined);
    const special = $derived(field.special?.some((option) => option.value === value));
    const baseline = $derived(Number(draft.baselineValue(field.key)));
    const ordinary = $derived(special ? Math.max(field.min, Math.min(field.max, baseline)) : Number(value));
    const help = $derived(unavailable ? unavailableReason : blockedReason || field.help || '');
</script>

<div class="parameter-field" use:measureWidth={{ scope: 'field' }} class:changed={field.key in draft.changes}>
    <div class="field-label"><AttributeHelp label={field.label} description={help} contextKey={field.key} /></div>
    {#if unavailable}
        <span class="parameter-unavailable"
            >{#if help}<AttributeHelp label={`${field.label}: Unavailable`} description={help}
                    >Unavailable</AttributeHelp
                >{:else}Unavailable{/if}</span
        >
    {:else if readOnlyText}
        <input class="editor-control" aria-label={field.label} value={readOnlyText} readonly disabled />
    {:else if field.boolean}
        <button
            type="button"
            role="switch"
            class="editor-switch"
            aria-label={field.label}
            aria-checked={value === true}
            disabled={disabled || unavailable}
            onclick={() => draft.set(field.key, value !== true)}><span></span></button
        >
    {:else if field.options}
        {#if field.key === 'midi_receive_channel' || field.key.startsWith('controls.')}
            <EditorAutocomplete
                label={field.label}
                value={unavailable ? undefined : Number(value)}
                options={field.options}
                disabled={disabled || unavailable}
                onchange={(next) => draft.set(field.key, next)}
            />
        {:else}
            <EditorChoice
                label={field.label}
                value={value === undefined ? undefined : Number(value)}
                options={field.options}
                disabled={disabled || unavailable}
                onchange={(next) => draft.set(field.key, next)}
            />
        {/if}
    {:else}
        <div class="field-value" class:with-mode={field.special}>
            {#if field.special}
                <EditorChoice
                    label={`${field.label} mode`}
                    value={special ? Number(value) : -999}
                    options={[{ value: -999, label: field.note ? 'Key' : 'Amount' }, ...field.special]}
                    segmented={false}
                    disabled={disabled || unavailable}
                    onchange={(next) =>
                        draft.set(field.key, next === -999 ? Math.max(field.min, Math.min(field.max, baseline)) : next)}
                />
            {/if}
            <EditorNumber
                label={field.label}
                value={unavailable ? undefined : ordinary}
                min={field.min}
                max={field.max}
                {slider}
                scale={field.scale ?? 1}
                unit={!unavailable && field.key === 'sample_eq_frequency'
                    ? eqFrequencyLabel(ordinary)
                    : field.note && !unavailable
                      ? noteName(ordinary)
                      : (field.unit ?? '')}
                disabled={disabled || unavailable || special}
                resetValue={baseline}
                onchange={(next) => draft.set(field.key, next)}
                onbegin={() => draft.beginGesture()}
                onend={() => draft.endGesture()}
                {oninvalid}
            />
        </div>
    {/if}
</div>
