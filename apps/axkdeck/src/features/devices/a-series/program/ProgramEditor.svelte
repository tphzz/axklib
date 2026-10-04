<script lang="ts">
    import { setContext, untrack } from 'svelte';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { EditorNavigation } from '../../../object-editor/navigation.svelte';
    import type { InspectorSelection, ProgramSampleSelectRow } from '../../../../lib/types';
    import EditorHeader from '../../../../lib/components/EditorHeader.svelte';
    import EditorTabs from '../../../../lib/components/EditorTabs.svelte';
    import ProgramAssignments from './ProgramAssignments.svelte';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import GraphPanel from '../../../object-editor/GraphPanel.svelte';
    import ExtendedParameterMarker from '../../../object-editor/ExtendedParameterMarker.svelte';
    import { editorScrollContext, rememberEditorScroll } from '../../../object-editor/editorScroll';
    import { ProgramDraft } from './draft.svelte';
    import { programTabs, programField, assignmentPages, type ProgramField as Field } from './fields';
    import ProgramField from './ProgramField.svelte';
    import ProgramEffects from './ProgramEffects.svelte';
    import ProgramWave from './ProgramWave.svelte';
    import ProgramEnvelope from './ProgramEnvelope.svelte';
    import ProgramMapping from './ProgramMapping.svelte';
    let {
        document: suppliedDocument,
        navigation,
        panelId,
        inactive = false,
        selection = null,
        assignmentQuery = '',
        onassignmentquerychange = () => {},
        onassignmentselect = () => {},
        multiPartContext = null,
    }: {
        document: ObjectEditorDocument;
        navigation: EditorNavigation;
        panelId: string;
        inactive?: boolean;
        selection?: InspectorSelection;
        assignmentQuery?: string;
        onassignmentquerychange?: (value: string) => void;
        onassignmentselect?: (row: ProgramSampleSelectRow) => void;
        multiPartContext?: { partLabel: string; programNumber: number } | null;
    } = $props();
    const document = untrack(() => suppliedDocument);
    const snapshot = $derived(
        document.detail?.editing?.profile === 'a-series/program' ? document.detail.editing : null,
    );
    const format = $derived(document.programFormat);
    const native = $derived(snapshot?.model === 'A3000');
    const tab = $derived(programTabs.find((item) => item.id === navigation.tab) ?? programTabs[0]!);
    const pages = $derived(tab.pages.filter((page) => !native || page.id !== 'stepwave'));
    const page = $derived(pages.find((item) => item.id === navigation.page) ?? pages[0]);
    const scroll = $derived({ positions: navigation.scrollPositions, key: page?.id ?? tab.id });
    setContext(editorScrollContext, () => scroll);
    const disabled = $derived(
        inactive || document.phase !== 'editable' || !snapshot?.editable || !!document.conflict || !format,
    );
    const values = $derived(document.draft.values);
    const draft = $derived(document.draft as ProgramDraft);
    const assignments = $derived(draft.assignments.filter((row) => row.name));
    const slot = $derived(Math.min(native ? 3 : 6, navigation.effectSlot));
    const assignment = $derived(assignments.find((row) => row.id === document.programAssignmentId) ?? assignments[0]);
    const field = (key: string): Field | undefined => (format ? programField(key, format, values) : undefined);
    const keys = $derived.by(() => {
        if (!format) return [];
        if (tab.id === 'easy-edit')
            return (assignmentPages[page?.id ?? 'mix'] ?? []).map((key) => `assignments.${assignment?.id ?? 0}.${key}`);
        if (page?.id === 'mix-portamento')
            return ['level', 'transpose', 'portamento.type', 'portamento.rate', 'portamento.time'];
        if (page?.id === 'sh') return ['lfo.sample_hold_speed'];
        if (page?.id === 'ad')
            return format.fields.filter((item) => item.key.startsWith('ad.')).map((item) => item.key);
        if (page?.id === 'program-lfo')
            return format.fields
                .filter((item) => item.key.startsWith('lfo.') && item.key !== 'lfo.sample_hold_speed')
                .map((item) => item.key);
        if (page?.id === 'stepwave') return ['step_wave.step_count', 'step_wave.slope'];
        return [];
    });
    const multiNote = $derived(
        !multiPartContext
            ? ''
            : tab.id === 'sample-select' || tab.id === 'easy-edit'
              ? 'Multi Part routing takes precedence over Receive Channel Assign.'
              : tab.id === 'effects' || tab.id === 'control' || page?.id === 'ad' || page?.id === 'sh'
                ? 'In Multi mode, these settings are used from the master Program.'
                : `Multi Part ${multiPartContext.partLabel}: editing the selected Program.`,
    );
</script>

{#if pages.length}
    <EditorHeader secondary>
        <EditorTabs
            secondary
            tabs={pages}
            active={page?.id ?? ''}
            onselect={(id) => (navigation.page = id)}
            label="Program subpages"
        />
        {#snippet tools()}<span class="editor-meta">{tab.label} · {pages.indexOf(page!) + 1} / {pages.length}</span
            >{/snippet}
    </EditorHeader>
{/if}
<div
    role="tabpanel"
    id={panelId}
    aria-labelledby={`${panelId}-${tab.id}`}
    class="program-panel"
    class:assignments={tab.id === 'sample-select'}
    class:graphical={page?.id === 'amp' || page?.id === 'routing'}
    use:rememberEditorScroll={scroll}
>
    {#if !snapshot || !format}
        <p role="status">Program parameter catalog is unavailable.</p>
    {:else if tab.id === 'sample-select'}
        <ProgramAssignments
            {document}
            {snapshot}
            {format}
            {disabled}
            {selection}
            query={assignmentQuery}
            onquerychange={onassignmentquerychange}
            onselect={onassignmentselect}
        />
    {:else if tab.id === 'effects'}
        <ProgramEffects
            {document}
            page={page?.id ?? 'routing'}
            {slot}
            onselect={(value) => (navigation.effectSlot = value)}
            {disabled}
        />
    {:else if page?.id === 'controllers'}
        <div class="controller-grid">
            {#each [1, 2, 3, 4] as controller}
                <section aria-label={`Control ${controller}`}>
                    <h3>Control {controller}</h3>
                    {#each ['device', 'function', 'type', 'range'] as key}
                        {@const control = field(`controllers.${controller}.${key}`)}{#if control}<ProgramField
                                {document}
                                field={control}
                                {disabled}
                            />{/if}
                    {/each}
                </section>
            {/each}
        </div>
    {:else if page?.id === 'channels'}
        <div class="fields">
            {#each native ? ['a'] : ['a', 'b'] as port}
                <section aria-label={`MIDI ${port.toUpperCase()} channels`}>
                    <h3>
                        MIDI {port.toUpperCase()}{#if port === 'b'}<ExtendedParameterMarker a5000Only />{/if}
                    </h3>
                    <div class="channels">
                        <strong>Channel</strong><strong>Controller reset</strong><strong>Note toggle</strong>
                        {#each Array.from({ length: 16 }, (_, i) => i + 1) as channel}
                            <span>{String(channel).padStart(2, '0')}</span>
                            {#each ['controller_reset', 'note_toggle'] as group}
                                {@const key = `${group}.${port}.${channel}`}
                                <button
                                    class="editor-switch"
                                    role="switch"
                                    aria-label={field(key)?.label}
                                    aria-checked={values[key] === true}
                                    disabled={disabled || values[key] === undefined}
                                    onclick={() => document.draft.set(key, values[key] !== true)}><span></span></button
                                >
                            {/each}
                        {/each}
                    </div>
                </section>
            {/each}
        </div>
    {:else}
        {#if tab.id === 'easy-edit'}
            <div class="assignment-picker">
                <span class="editor-meta">Sample/Bank</span><EditorChoice
                    label="Sample/Bank"
                    segmented={false}
                    value={assignment?.id}
                    options={assignments.map((row) => ({
                        value: row.id,
                        label: `${row.name} (${row.kind === 'SBAC' ? 'Bank' : 'Sample'})${assignments.filter((other) => other.kind === row.kind && other.name === row.name).length > 1 ? ` · ${row.id + 1}` : ''}`,
                    }))}
                    disabled={inactive || !assignments.length}
                    onchange={(value) => (document.programAssignmentId = value)}
                />
            </div>
            {#if !assignment}<p>No assigned Samples or Sample Banks.</p>{/if}
        {/if}
        {#if page?.id === 'program-lfo'}<ProgramWave {document} {disabled} />{/if}
        {#if page?.id === 'amp' && assignment}
            <div class="graph-body">
                <GraphPanel
                    label="Program amplitude envelope"
                    layoutKey="program-amp"
                    controlsMinimum={300}
                    stackBelow={740}
                    graphMinHeight={250}
                >
                    {#snippet graph()}<ProgramEnvelope
                            {document}
                            {snapshot}
                            {assignment}
                            disabled={disabled || assignment.kind === 'UNKNOWN'}
                        />{/snippet}
                    {#snippet controls()}
                        <h3>Rate offsets</h3>
                        <div class="rate-fields">
                            {#each keys as key}
                                {@const control = field(key)}
                                {#if control}<ProgramField
                                        {document}
                                        field={control}
                                        disabled={disabled || assignment.kind === 'UNKNOWN'}
                                    />{/if}
                            {/each}
                        </div>
                    {/snippet}
                </GraphPanel>
            </div>
        {:else}<div class="fields">
                {#each keys as key}
                    {@const control = field(key)}{#if control}<ProgramField
                            {document}
                            field={control}
                            disabled={disabled || (tab.id === 'easy-edit' && assignment?.kind === 'UNKNOWN')}
                        />{/if}
                {/each}
            </div>{/if}
        {#if page?.id === 'stepwave'}<ProgramWave {document} step {disabled} />{/if}
        {#if page?.id === 'range' && assignment}
            <ProgramMapping {document} {snapshot} {assignment} disabled={disabled || assignment.kind === 'UNKNOWN'} />
        {/if}
    {/if}
</div>
<footer role="status" title={document.validation || document.status || multiNote || snapshot?.reason || 'Ready'}>
    {document.validation ||
        document.status ||
        multiNote ||
        snapshot?.reason ||
        (document.draft.dirty ? 'Unsaved changes' : 'Ready')}
</footer>

<style>
    .program-panel {
        flex: 1;
        min-height: 0;
        overflow: auto;
        padding: var(--density-panel-padding, 6px);
    }
    .program-panel.assignments {
        display: flex;
        flex-direction: column;
        overflow: hidden;
        padding: 0;
    }
    .program-panel.graphical {
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }
    .graph-body {
        flex: 1;
        min-height: 0;
    }
    .rate-fields {
        display: grid;
        gap: 6px;
    }
    .fields,
    .controller-grid {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(min(100%, 330px), 1fr));
        gap: 6px 18px;
        margin: 8px 0;
    }
    .assignment-picker {
        display: grid;
        grid-template-columns: 70px minmax(0, 1fr);
        align-items: center;
        max-width: 420px;
        margin-bottom: 10px;
    }
    .controller-grid section {
        display: flex;
        flex-direction: column;
        gap: 6px;
    }
    h3,
    strong {
        font-size: 11px;
        font-weight: 600;
    }
    h3 {
        margin: 0 0 6px;
    }
    .channels {
        display: grid;
        grid-template-columns: 70px 1fr 1fr;
        align-items: center;
        gap: 6px;
    }
    .channels strong {
        font-size: 10px;
        color: var(--color-text-muted);
    }
    footer {
        flex: none;
        height: 26px;
        line-height: 25px;
        border-top: 1px solid var(--color-border);
        padding: 0 8px;
        font-size: 10px;
        color: var(--color-text-muted);
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }
    p {
        font-size: 11px;
        color: var(--color-text-muted);
    }
</style>
