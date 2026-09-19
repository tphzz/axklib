<script lang="ts">
    import GraphPanel from '../../../object-editor/GraphPanel.svelte';
    import ParameterGroups from '../../../object-editor/ParameterGroups.svelte';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { SamplePage, SampleField } from './fields';
    import { pageGroups } from './pageGroups';
    import { sampleView } from './view.svelte';
    import ParameterField from './ParameterField.svelte';
    import ResponseGraph from './ResponseGraph.svelte';
    import SampleLfoGraph from './SampleLfoGraph.svelte';
    import SampleEqGraph from './SampleEqGraph.svelte';
    import SampleFilterGraph from './SampleFilterGraph.svelte';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import Icon from '../../../../lib/components/Icon.svelte';
    import { parameterBlockReason } from './parameterAvailability';
    let { document, page, disabled }: { document: ObjectEditorDocument; page: SamplePage; disabled: boolean } =
        $props();
    const view = $derived(sampleView(document));
    const scaling = $derived(page.id.endsWith('-scaling'));
    const graphFields = $derived(page.fields.filter((field) => field.key.includes('_scaling_')));
    const blocked = $derived(document.detail?.editing?.blockedParameters ?? []);
    const groups = $derived(
        scaling
            ? [
                  {
                      title: 'Key scaling',
                      fields: [graphFields[0]!, graphFields[2]!, graphFields[1]!, graphFields[3]!],
                  },
                  ...(page.fields.length > 4 ? [{ title: 'Velocity', fields: page.fields.slice(4) }] : []),
              ]
            : pageGroups(page),
    );
    const title = $derived(
        scaling ? (page.id === 'level-scaling' ? 'Key to level' : 'Key to cutoff') : `${page.label} envelope`,
    );
    function resetGraph() {
        document.draft.patch(
            Object.fromEntries(
                graphFields
                    .filter(
                        (field) =>
                            !blocked.includes(field.key) && document.draft.baselineValue(field.key) !== undefined,
                    )
                    .map((field) => [field.key, document.draft.baselineValue(field.key)!]),
            ),
        );
    }
    function selectField(key: string) {
        const group = groups.find((item) => item.fields.some((field) => field.key === key));
        if (group) view.parameterGroups[page.id] = group.title;
    }
</script>

{#snippet graph()}
    {#if page.id === 'lfo'}<SampleLfoGraph {document} />
    {:else if page.id === 'sample-eq'}<SampleEqGraph {document} {disabled} />
    {:else if page.id === 'filter'}<SampleFilterGraph {document} {disabled} />
    {:else}
        {#snippet graphTitle()}
            {#if scaling}<h3 class="editor-heading">{title}</h3>
            {:else}<AttributeHelp
                    label={title}
                    description="Relative envelope shape, not calibrated time. Higher rates shorten transitions; maximum Amplitude release is immediate. Horizontal handles edit rates; vertical movement edits supported levels. Fit or zoom out reveals stages outside the current view. The sustain interval is illustrative. Amplitude Hold starts at full level. Rate 2 uses schematic spacing for its device-specific attack variation."
                />{/if}
        {/snippet}
        {#snippet graphTools()}
            {#if scaling}<button
                    class="editor-icon"
                    title="Reset scaling to saved values"
                    aria-label="Reset scaling"
                    disabled={disabled ||
                        !graphFields.some(
                            (field) => !blocked.includes(field.key) && field.key in document.draft.changes,
                        )}
                    onclick={resetGraph}><Icon name="undo" size={14} /></button
                >{/if}
        {/snippet}
        <ResponseGraph
            draft={document.draft}
            page={page.id}
            {disabled}
            {blocked}
            onselect={selectField}
            title={graphTitle}
            tools={graphTools}
        />
    {/if}
{/snippet}
{#snippet fieldControl(field: SampleField)}
    {@const programSpeed = field.key === 'lfo.speed' && document.draft.values['lfo.wave'] === 3}
    {@const shelfWidth = field.key === 'sample_eq_width_tenths' && document.draft.values.sample_eq_type !== 0}
    {@const filterInactive =
        page.id === 'filter' &&
        field.key !== 'filter_type' &&
        (document.draft.values.filter_type === 0 ||
            (field.key === 'filter_cutoff_distance' && Number(document.draft.values.filter_type) < 10))}
    <ParameterField
        field={programSpeed
            ? { ...field, help: 'Sample & Hold speed is controlled by the Program.' }
            : shelfWidth
              ? { ...field, help: 'Shelf EQ uses a fixed response width. The stored Peak/Dip width is retained.' }
              : filterInactive
                ? {
                      ...field,
                      help: 'This setting is inactive for the selected filter type. Its stored value is retained.',
                  }
                : field}
        draft={document.draft}
        unavailableReason={document.detail?.editing?.unavailableParameters[field.key]?.message}
        disabled={disabled || blocked.includes(field.key) || shelfWidth || filterInactive}
        blockedReason={parameterBlockReason(field.key, blocked)}
        readOnlyText={programSpeed ? 'Program' : undefined}
        oninvalid={(message) => (document.inputErrors = { ...document.inputErrors, [field.key]: message })}
    />
{/snippet}
{#snippet controls()}
    <ParameterGroups {groups} field={fieldControl} bind:selected={view.parameterGroups[page.id]} />
{/snippet}
<GraphPanel {graph} {controls} label={page.label} />
