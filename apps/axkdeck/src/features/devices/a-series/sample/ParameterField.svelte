<script lang="ts">
    import { getContext } from 'svelte';
    import { BankDraft } from '../bank/draft.svelte';
    import BankFieldControl from '../bank/BankFieldControl.svelte';
    import ExtendedParameterMarker from '../../../object-editor/ExtendedParameterMarker.svelte';
    import { formatField, sampleFormatContext, type SampleFormatContext } from './formatCapabilities';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import ParameterLabel from '../../../object-editor/ParameterLabel.svelte';
    import EditorNumber from '../../../object-editor/EditorNumber.svelte';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import EditorAutocomplete from '../../../object-editor/EditorAutocomplete.svelte';
    import type { SampleField } from './fields';
    import type { EditorDraft } from '../../../object-editor/draft.svelte';
    import { noteName } from './geometry';
    import { eqFrequencyLabel } from './eqModel';
    import { measureWidth } from '../../../object-editor/measureWidth';
    let {
        field: suppliedField,
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
    const format = getContext<SampleFormatContext | undefined>(sampleFormatContext);
    const snapshot = $derived(format?.());
    const field = $derived(formatField(suppliedField, snapshot));
    const capability = $derived(snapshot?.parameterCapabilities[field.key]);
    const nativeEq = $derived(field.key === 'sample_eq_type' && snapshot?.sampleFormat.format === 'A3000_188');
    const value = $derived(nativeEq ? 0 : draft.values[field.key]);
    const unavailable = $derived(value === undefined);
    const special = $derived(field.special?.some((option) => option.value === value));
    const baseline = $derived(Number(draft.baselineValue(field.key)));
    const ordinary = $derived(special ? Math.max(field.min, Math.min(field.max, baseline)) : Number(value));
    const help = $derived(
        nativeEq
            ? 'A3000 Sample EQ uses Peak/Dip. Convert explicitly to a4k/a5k format for shelf EQ.'
            : blockedReason || capability?.reason || (unavailable ? unavailableReason : field.help) || '',
    );
    const locked = $derived(disabled || capability?.editable === false || nativeEq);
</script>

<div class="parameter-field" use:measureWidth={{ scope: 'field' }} class:changed={field.key in draft.changes}>
    <div class="field-label">
        <ParameterLabel
            label={field.label}
            description={help}
            parameter={field.key}
            format={(value) =>
                value === undefined
                    ? 'Unavailable'
                    : (field.options?.find((option) => option.value === Number(value))?.label ??
                      field.special?.find((option) => option.value === value)?.label ??
                      (typeof value === 'boolean'
                          ? value
                              ? 'On'
                              : 'Off'
                          : field.note
                            ? `${value} (${noteName(value)})`
                            : `${value / (field.scale ?? 1)}${field.unit ? ` ${field.unit}` : ''}`))}
        />
        {#if field.extended}<ExtendedParameterMarker />{/if}
    </div>
    {#if draft instanceof BankDraft && (draft.unit(field.key) || snapshot?.blockedParameters.includes(field.key))}
        <BankFieldControl {field} {draft} disabled={locked || !!readOnlyText} {slider} {oninvalid} />
    {:else if unavailable}
        <span class="parameter-unavailable"
            >{#if help}<AttributeHelp label={`${field.label}: Unavailable`} description={help}
                    >Unavailable</AttributeHelp
                >{:else}Unavailable{/if}</span
        >
    {:else if nativeEq}
        <input class="editor-control" aria-label={field.label} value="Peak/Dip" readonly disabled />
    {:else if readOnlyText}
        <input class="editor-control" aria-label={field.label} value={readOnlyText} readonly disabled />
    {:else if field.boolean && !field.options}
        <button
            type="button"
            role="switch"
            class="editor-switch"
            aria-label={field.label}
            aria-checked={value === true}
            disabled={locked || unavailable}
            onclick={() => draft.set(field.key, value !== true)}><span></span></button
        >
    {:else if field.options}
        {#if field.key === 'midi_receive_channel' || field.key.startsWith('controls.')}
            <EditorAutocomplete
                label={field.label}
                value={unavailable ? undefined : Number(value)}
                options={field.options}
                disabled={locked || unavailable}
                onchange={(next) => draft.set(field.key, field.boolean ? Boolean(next) : next)}
            />
        {:else}
            <EditorChoice
                label={field.label}
                value={value === undefined ? undefined : Number(value)}
                options={field.options}
                disabled={locked || unavailable}
                onchange={(next) => draft.set(field.key, field.boolean ? Boolean(next) : next)}
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
                    disabled={locked || unavailable}
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
                disabled={locked || unavailable || special}
                resetValue={baseline}
                onchange={(next) => draft.set(field.key, next)}
                onbegin={() => draft.beginGesture()}
                onend={() => draft.endGesture()}
                {oninvalid}
            />
        </div>
    {/if}
</div>
