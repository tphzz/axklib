<script lang="ts">
    import '../features/object-editor/editor.css';
    import KeyboardMapping from '../features/object-editor/KeyboardMapping.svelte';
    import { noteName } from '../features/devices/a-series/sample/geometry';
    import type {
        KeyboardMappingPreview,
        KeyboardRange,
        KeyboardZone,
    } from '../features/object-editor/keyboardMapping';
    import { createMappingPreview } from '../features/program-mapping/preview';

    const query = new URLSearchParams(location.search);
    const layered = query.has('overlap');
    const grouped = query.has('group');
    const dense = query.has('dense');
    const audition = query.has('audition');
    const live = query.has('live');
    let preview = $state.raw<KeyboardMappingPreview>();
    let project: ReturnType<typeof createMappingPreview>;
    let original: KeyboardRange;
    let velocity = $state(100);
    const count = Math.max(1, Math.min(512, Number(query.get('count')) || 8));
    let selected = $state('zone-0');
    let disabled = $state(false);
    let limits = $state<KeyboardRange>({ low: 0, high: 15, velocityLow: 0, velocityHigh: 127 });
    const initial: KeyboardZone[] = Array.from({ length: count }, (_, index) => ({
        id: `zone-${index}`,
        label: `Organ ${index + 1} - sustained factory Sample with a long name`,
        low: dense ? 0 : Math.floor((index * 128) / count),
        high: dense ? 127 : Math.max(Math.floor((index * 128) / count), Math.floor(((index + 1) * 128) / count) - 1),
        velocityLow: dense ? index % 64 : 0,
        velocityHigh: dense ? 64 + (index % 64) : 127,
        root: dense ? 60 : Math.min(127, Math.floor(((index + 0.5) * 128) / count)),
    }));
    if (layered)
        initial.push({ ...initial[0]!, id: 'overlap', label: 'Overlapping Sample', velocityLow: 32, velocityHigh: 95 });
    let source = $state.raw(initial);
    const zones = $derived(
        source.map((zone) => ({
            ...zone,
            source: live ? undefined : zone,
            selectionId: Number(zone.id.slice(5)),
            selected: zone.id === selected || (grouped && selected === 'zone-0' && zone.id === 'zone-1'),
        })),
    );
    function select(id: string) {
        selected = id;
        const zone = source.find((zone) => zone.id === id)!;
        limits = { low: zone.low, high: zone.high, velocityLow: zone.velocityLow, velocityHigh: zone.velocityHigh };
    }
    function begin() {
        original = { ...limits };
        project = live
            ? createMappingPreview({
                  role: 'sample',
                  zones,
                  limits,
                  selectionId: Number(selected.slice(5)),
                  overrides: [],
              })
            : null;
    }
    function change(range: KeyboardRange) {
        limits = range;
        preview = project?.(range);
    }
    function end(cancelled = false) {
        if (live) {
            if (cancelled) limits = original;
            else if (preview) {
                const changed = new Map(preview.zones.map((zone) => [zone.id, zone]));
                source = source.map((zone) => changed.get(zone.id) ?? zone);
            }
        }
        project = null;
        preview = undefined;
    }
</script>

<main class="device-editor fixture">
    <div class="compact">
        <KeyboardMapping
            {zones}
            {limits}
            {preview}
            {disabled}
            formatNote={noteName}
            onselect={select}
            onchange={change}
            onbegin={begin}
            onend={end}
            onpress={audition ? () => {} : undefined}
            {velocity}
            onvelocity={(value) => (velocity = value)}
        >
            {#snippet tools()}
                <div class="fixture-tools">
                    <button class="editor-action" onclick={() => (disabled = !disabled)}>Read-only</button>
                    <button
                        class="editor-action"
                        onclick={() => (limits = { low: 0, high: 127, velocityLow: 0, velocityHigh: 127 })}
                        >Wide limits</button
                    >
                    <button
                        class="editor-action"
                        onclick={() => (limits = { low: 64, high: 64, velocityLow: 64, velocityHigh: 64 })}
                        >Thin limits</button
                    >
                </div>
            {/snippet}
        </KeyboardMapping>
    </div>
    <div class="full">
        <KeyboardMapping
            {zones}
            {limits}
            {preview}
            {disabled}
            mode="mapping"
            formatNote={noteName}
            onselect={select}
            onchange={change}
            onbegin={begin}
            onend={end}
            onpress={audition ? () => {} : undefined}
            {velocity}
            onvelocity={(value) => (velocity = value)}
            rangeLabel="Editable limits"
        />
    </div>
    <output hidden data-presentation>{JSON.stringify({ selected, limits, disabled })}</output>
</main>

<style>
    .fixture {
        display: grid;
        grid-template-columns: minmax(0, 1fr);
        grid-template-rows: auto minmax(0, 1fr);
        gap: 8px;
        height: 100dvh;
        padding: 12px;
        background: var(--color-panel);
        color: var(--color-text);
    }
    .full {
        min-height: 0;
    }
    .fixture-tools {
        display: flex;
        gap: 4px;
    }
    .fixture-tools button {
        height: 26px;
        padding: 3px 6px;
        border: 1px solid var(--color-border);
        border-radius: 3px;
        background: var(--color-panel-deep);
        color: var(--color-text);
        font: 11px var(--font-sans);
    }
</style>
