<script lang="ts">
    import EnvelopeGraph from '../features/devices/a-series/sample/EnvelopeGraph.svelte';
    import type { SampleEnvelope } from '../features/devices/a-series/sample/envelope';
    import ParameterGraph from '../features/object-editor/ParameterGraph.svelte';
    import { EditorDraft, type EditorValues } from '../features/object-editor/draft.svelte';
    import { BankDraft } from '../features/devices/a-series/bank/draft.svelte';
    import { bankEditorUnits } from './bankEditorFixture';

    let kind = $state<SampleEnvelope>('feg');
    let bank = $state(true);
    let disabled = $state(false);
    let blocked = $state(false);
    let field = $state('');
    let generation = $state(0);
    function values(): EditorValues {
        return Object.fromEntries(
            ['aeg', 'feg', 'peg'].flatMap((prefix) => [
                [`${prefix}.attack_rate`, 127],
                [`${prefix}.decay_rate`, 127],
                [`${prefix}.release_rate`, 127],
                [`${prefix}.init_level`, 0],
                [`${prefix}.attack_level`, 0],
                [`${prefix}.sustain_level`, prefix === 'aeg' ? 127 : 0],
                [`${prefix}.release_level`, 0],
                [`${prefix}.attack_mode`, 0],
            ]),
        );
    }
    function newDraft(): EditorDraft {
        const initial = values();
        if (!bank) return new EditorDraft(initial);
        const draft = new BankDraft(initial, bankEditorUnits(true, new Set()));
        draft.member = initial;
        return draft;
    }
    let draft = $state(newDraft());
    function reset(nextKind = kind, nextBank = bank) {
        kind = nextKind;
        bank = nextBank;
        field = '';
        disabled = false;
        blocked = false;
        draft = newDraft();
        generation += 1;
    }
    let selected = $state('first');
    let changes = $state<Record<string, { x: number; y: number }>>({});
    const overlap = [
        { id: 'first', label: 'First', readout: '0', x: 0, y: 0.5, horizontal: true, vertical: true },
        { id: 'second', label: 'Second', readout: '0', x: 0, y: 0.5, horizontal: true, vertical: true },
        { id: 'edge', label: 'Edge', readout: '0', x: 1, y: 0, horizontal: true, vertical: true },
    ];
</script>

<nav aria-label="Envelope fixture controls">
    {#each ['aeg', 'feg', 'peg'] as name}<button onclick={() => reset(name as SampleEnvelope)}>{name}</button>{/each}
    <button onclick={() => reset(kind, false)}>Sample</button>
    <button onclick={() => reset(kind, true)}>Bank</button>
    <button onclick={() => reset()}>Reset</button>
    <button onclick={() => draft.undo()}>Undo</button>
    <button onclick={() => (disabled = !disabled)}>Disabled</button>
    <button onclick={() => (blocked = !blocked)}>Block initial</button>
</nav>
<section class="device-editor" data-envelope-fixture>
    {#key generation}
        {#snippet title()}<h3 class="editor-heading">
                {kind === 'aeg' ? 'Amplitude' : kind === 'feg' ? 'Filter' : 'Pitch'} envelope
            </h3>{/snippet}
        <EnvelopeGraph
            {draft}
            {kind}
            {disabled}
            {title}
            blocked={blocked ? [`${kind}.init_level`] : []}
            onselect={(key) => (field = key)}
        />
    {/key}
</section>
<output data-envelope-state
    >{JSON.stringify({
        kind,
        bank,
        field,
        values: draft.values,
        changes: draft.changes,
        dirty: draft.dirty,
        canUndo: draft.canUndo,
    })}</output
>
<section class="device-editor" data-overlap-fixture>
    {#snippet tools()}
        <select aria-label="Exact overlap stage" bind:value={selected}>
            {#each overlap as handle}<option value={handle.id}>{handle.label}</option>{/each}
        </select>
    {/snippet}
    <ParameterGraph
        label="Exact overlap test"
        handles={overlap}
        traces={[{ id: 'overlap', points: overlap }]}
        ticks={[1, 0, -1]}
        axis={[
            { x: 0, label: 'Start' },
            { x: 1, label: 'End' },
        ]}
        bind:selected
        retainReadout
        {tools}
        onchange={(id, x, y) => (changes = { ...changes, [id]: { x, y } })}
    />
</section>
<output data-overlap-state>{JSON.stringify({ selected, changes })}</output>

<style>
    :global(body) {
        margin: 0;
    }
    nav {
        display: flex;
        flex-wrap: wrap;
        gap: 4px;
        padding: 8px;
    }
    nav button {
        font-size: 11px;
    }
    section {
        display: flex;
        flex-direction: column;
        min-width: 0;
        height: 250px;
        padding: 8px;
        background: var(--color-panel);
    }
    output {
        display: block;
        height: 28px;
        overflow: hidden;
        font-size: 10px;
    }
    select {
        font-size: 11px;
        width: 112px;
        height: 24px;
        padding: 0 5px;
    }
</style>
