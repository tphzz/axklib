<script lang="ts">
    import { sampleSnapshot } from '../../../../lib/objectEditing';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { SamplePage, SampleField } from './fields';
    import { pageGroups } from './pageGroups';
    import ParameterField from './ParameterField.svelte';
    import VelocityRange from './VelocityRange.svelte';
    import SampleMapping from './SampleMapping.svelte';
    import GraphicalSamplePage from './GraphicalSamplePage.svelte';
    import { parameterBlockReason, parameterInactiveReason } from './parameterAvailability';
    import ControlPage from './ControlPage.svelte';
    import { measureWidth } from '../../../object-editor/measureWidth';
    import { initialEditorWidth } from '../../../object-editor/editorWidth';
    import { BankDraft } from '../bank/draft.svelte';
    let { document, page, disabled }: { document: ObjectEditorDocument; page: SamplePage; disabled: boolean } =
        $props();
    const graphical = $derived(
        page.id.endsWith('-scaling') || ['aeg', 'feg', 'peg', 'lfo', 'filter', 'sample-eq'].includes(page.id),
    );
    const blocked = (field: SampleField) =>
        disabled ||
        (sampleSnapshot(document.detail)?.blockedParameters.includes(field.key) ?? true) ||
        (!(document.draft instanceof BankDraft) && !!parameterInactiveReason(field.key, document.draft.values));
    let width = $state(initialEditorWidth());
    const columns = $derived(width >= 984 ? 3 : width >= 652 ? 2 : 1);
    const groups = $derived(pageGroups(page));
    const tracks = $derived(
        columns === 3
            ? groups
                  .map((group) =>
                      group.fields.every((field) => document.draft.values[field.key] === undefined)
                          ? 'minmax(0, 240px)'
                          : 'minmax(0, 380px)',
                  )
                  .join(' ')
            : `repeat(${columns}, minmax(0, 380px))`,
    );
</script>

{#if graphical}
    {#key page.id}<GraphicalSamplePage {document} {page} {disabled} />{/key}
{:else if page.id === 'control'}
    <ControlPage {document} {page} {disabled} />
{:else}
    <div
        class="parameter-groups"
        data-page={page.id}
        data-columns={columns}
        use:measureWidth={{ scope: 'form', change: (value) => (width = value) }}
        style:grid-template-columns={tracks}
    >
        {#each groups as group}
            <section>
                <h3 class="editor-heading">{group.title}</h3>
                {#if page.id === 'velocity' && group.title === 'Velocity range'}<VelocityRange
                        {document}
                        fields={group.fields}
                        {disabled}
                    />{:else}
                    {#each group.fields.filter((field) => !(document.draft instanceof BankDraft && page.id === 'mix-key' && ['root_key', 'key_low', 'key_high'].includes(field.key))) as field (field.key)}
                        <ParameterField
                            {field}
                            draft={document.draft}
                            unavailableReason={sampleSnapshot(document.detail)?.unavailableParameters[field.key]
                                ?.message}
                            disabled={blocked(field)}
                            blockedReason={parameterInactiveReason(field.key, document.draft.values) ||
                                parameterBlockReason(field.key, sampleSnapshot(document.detail)!)}
                            oninvalid={(message) =>
                                (document.inputErrors = { ...document.inputErrors, [field.key]: message })}
                        />
                    {/each}
                {/if}
            </section>
        {/each}
        {#if page.id === 'mix-key'}
            <div class="mapping-row"><SampleMapping {document} {disabled} /></div>
        {/if}
    </div>
{/if}

<style>
    .parameter-groups {
        display: grid;
        gap: var(--density-section-gap, 12px);
        align-content: start;
        justify-content: start;
    }
    .mapping-row {
        grid-column: 1 / -1;
        min-width: 0;
        /* The mapping component already supplies its vertical spacing. */
        margin-top: calc(-1 * var(--density-section-gap, 12px));
    }
    section {
        min-width: 0;
    }
    h3 {
        margin: 0 0 6px;
    }
    section :global(.parameter-field) {
        margin-bottom: 0;
    }
    [data-columns='2'][data-page='mix-key'] section:nth-child(2) {
        grid-column: 2;
        grid-row: 1 / 3;
    }
    [data-columns='2'][data-page='mix-key'] section:nth-child(3) {
        grid-column: 1;
    }
</style>
