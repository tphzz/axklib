<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { ProgramField } from './fields';
    import EditorNumber from '../../../object-editor/EditorNumber.svelte';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import EditorAutocomplete from '../../../object-editor/EditorAutocomplete.svelte';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import ExtendedParameterMarker from '../../../object-editor/ExtendedParameterMarker.svelte';
    import { measureWidth } from '../../../object-editor/measureWidth';
    import { noteName } from '../sample/geometry';
    let {
        document,
        field,
        disabled = false,
    }: { document: ObjectEditorDocument; field: ProgramField; disabled?: boolean } = $props();
    const value = $derived(document.draft.values[field.key]);
    const locked = $derived(disabled || !!field.reason || value === undefined);
    const earlier = $derived(document.programCatalog?.formats.find((item) => item.model === 'A3000'));
    const earlierDomain = $derived(
        earlier?.fields.find(
            (item) => item.key === (field.nativeKey ?? field.key).replace(/^assignments\.\d+\./, 'assignments.*.'),
        ),
    );
    const later = $derived(document.programFormat?.model !== 'A3000' && !!earlier);
    const extended = $derived(
        field.extended ?? (later && (!earlierDomain || field.min < earlierDomain.min || field.max > earlierDomain.max)),
    );
    const options = $derived(
        field.options?.map((item) => ({
            ...item,
            extended:
                item.extended ||
                (later &&
                    (!earlierDomain ||
                        item.value < earlierDomain.min ||
                        item.value > earlierDomain.max ||
                        (!!earlierDomain.allowedValues && !earlierDomain.allowedValues.includes(item.value)))),
        })),
    );
    const help = $derived(
        field.reason ||
            (field.key.includes('_offset')
                ? 'Signed adjustment to the assigned Sample or Sample Bank. Zero leaves its value unchanged.'
                : ''),
    );
</script>

<div class="parameter-field" class:changed={field.key in document.draft.changes} use:measureWidth={{ scope: 'field' }}>
    <div class="field-label">
        {#if help}<AttributeHelp label={field.label} description={help}>{field.label}</AttributeHelp
            >{:else}{field.label}{/if}
        {#if extended || field.a5000Only}<ExtendedParameterMarker a5000Only={field.a5000Only} />{/if}
    </div>
    {#if options}
        {#if options.length > 16}
            <EditorAutocomplete
                label={field.label}
                value={typeof value === 'number' ? value : undefined}
                {options}
                disabled={locked}
                onchange={(value) => document.draft.set(field.key, value)}
            />
        {:else}
            <EditorChoice
                label={field.label}
                value={typeof value === 'number' ? value : undefined}
                {options}
                disabled={locked}
                onchange={(value) => document.draft.set(field.key, value)}
            />
        {/if}
    {:else if field.boolean}
        <button
            type="button"
            role="switch"
            class="editor-switch"
            aria-label={field.label}
            aria-checked={value === true}
            disabled={locked}
            onclick={() => document.draft.set(field.key, value !== true)}><span></span></button
        >
    {:else}
        <EditorNumber
            label={field.label}
            value={typeof value === 'number' ? value : undefined}
            min={field.min}
            max={field.max}
            unit={field.note && typeof value === 'number' ? noteName(value) : ''}
            disabled={locked}
            resetValue={Number(document.draft.baselineValue(field.key))}
            onchange={(value) => document.draft.set(field.key, value)}
            onbegin={() => document.draft.beginGesture()}
            onend={() => document.draft.endGesture()}
            oninvalid={(message) => (document.inputErrors = { ...document.inputErrors, [field.key]: message })}
        />
    {/if}
</div>
