<script lang="ts">
    import { onDestroy } from 'svelte';
    import type { SampleWaveformPreview } from '../../../../lib/types';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import EditorNumber from '../../../object-editor/EditorNumber.svelte';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import Icon from '../../../../lib/components/Icon.svelte';
    import { editorAudio } from '../../../object-editor/audioContext';
    import { sampleView } from './view.svelte';
    import { clamp, markerBounds, markerValues, moveMarker, nearestCrossing } from './geometry';
    import WaveCanvas from './WaveCanvas.svelte';
    let {
        document,
        preview,
        disabled,
        onseek = () => {},
    }: {
        document: ObjectEditorDocument;
        preview: SampleWaveformPreview;
        disabled: boolean;
        onseek?: (frame: number) => void;
    } = $props();
    const audio = editorAudio();
    const view = $derived(sampleView(document));
    let host = $state<HTMLDivElement>();
    let width = $state(800);
    const frames = $derived(Math.max(1, document.detail!.editing!.maximumFrames));
    const values = $derived(document.draft.values);
    const markers = $derived(markerValues(values));
    const bounds = $derived(markerBounds(values, frames));
    const lanes = $derived(preview.preview?.lanes ?? []);
    const rate = $derived(lanes[0]?.sampleRate ?? view.source?.sampleRate ?? 44100);
    const start = $derived(view.pan * (frames - frames / view.zoom));
    const end = $derived(start + frames / view.zoom);
    const names = ['Wave start', 'Wave end', 'Loop start', 'Loop end'];
    const labels = ['START', 'END', 'LOOP START', 'LOOP END'];
    const keys = ['playback.start_frame', 'playback.length_frames', 'loop_start_frame', 'loop_length_frames'];
    const position = (frame: number) => ((frame - start) / (end - start)) * 100;
    const format = (frame: number) =>
        view.units ? `${((frame / rate) * 1000).toFixed(1)}` : Math.round(frame).toLocaleString('en-US');
    function blocked(index: number) {
        const snapshot = document.detail!.editing!;
        return (
            disabled ||
            !Number.isFinite(markers[index]) ||
            (index < 2 && !snapshot.canEditPlayback) ||
            [keys[index]!, ...(index % 2 === 0 ? [keys[index + 1]!] : [])].some((key) =>
                snapshot.blockedParameters.includes(key),
            )
        );
    }
    function change(index: number, frame: number, snap = false) {
        if (blocked(index)) return;
        const [min, max] = bounds[index]!;
        const pcm = view.source?.lanes[0]?.getChannelData(0);
        const next = snap && view.snap && pcm ? nearestCrossing(pcm, frame, min, max) : frame;
        document.draft.patch(moveMarker(values, index, next, frames));
    }
    function drag(event: PointerEvent, index: number) {
        if (blocked(index)) return;
        event.preventDefault();
        const target = event.currentTarget as HTMLButtonElement;
        target.focus();
        target.setPointerCapture(event.pointerId);
        document.draft.beginGesture();
        const move = (next: PointerEvent) => {
            if (!host) return;
            const rect = host.getBoundingClientRect();
            change(index, start + ((next.clientX - rect.left) / rect.width) * (end - start), true);
        };
        const finish = () => {
            target.removeEventListener('pointermove', move);
            for (const name of ['pointerup', 'pointercancel', 'lostpointercapture'])
                target.removeEventListener(name, finish);
            document.draft.endGesture();
        };
        target.addEventListener('pointermove', move);
        for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) target.addEventListener(name, finish);
    }
    async function zoom(next: number) {
        if (next > 1 && !(await view.load(document, audio))) return;
        view.zoom = clamp(next, 1, 8);
        if (view.zoom === 1) view.pan = 0;
    }
    onDestroy(() => document.draft.endGesture());
</script>

<section class="waveform-page" aria-label="Waveform editor">
    <div class="editor-toolbar">
        <h3 class="editor-heading">Waveform</h3>
        <span class="editor-meta total">{frames.toLocaleString('en-US')} samples</span>
        <span class="editor-spacer"></span>
        <AttributeHelp
            label="Snap"
            description="Snap dragged markers to a nearby zero crossing in the mono or left channel. Stereo boundaries move together. Numeric entry remains exact."
        />
        <button
            role="switch"
            aria-label="Zero-cross snap"
            aria-checked={view.snap}
            class="editor-switch"
            disabled={view.loading}
            onclick={async () => {
                if (view.snap) view.snap = false;
                else if (await view.load(document, audio)) view.snap = true;
            }}><span></span></button
        >
        <button
            class="editor-icon"
            aria-label="Zoom out"
            title="Zoom out"
            disabled={view.zoom === 1}
            onclick={() => void zoom(view.zoom / 2)}>-</button
        >
        <span class="editor-meta zoom">{view.zoom}x</span>
        <button
            class="editor-icon"
            aria-label="Zoom in"
            title="Zoom in"
            disabled={view.zoom === 8 || view.loading}
            onclick={() => void zoom(view.zoom * 2)}>+</button
        >
        <button class="editor-icon" aria-label="Fit waveform" title="Fit waveform" onclick={() => void zoom(1)}
            ><Icon name="grid" size={13} /></button
        >
    </div>
    <div class="wave-ruler" aria-label={`Position in ${view.units ? 'milliseconds' : 'samples'}`}>
        {#each Array.from({ length: 9 }, (_, i) => start + (i * (end - start)) / 8) as frame, i}<span
                class:minor={i % 2 === 1}>{format(frame)}</span
            >{/each}
    </div>
    <div class="wave-surface" bind:this={host} bind:clientWidth={width}>
        <div class="wave-lanes">
            {#each lanes as lane, index}
                <div class="wave-lane">
                    <span class="lane-role">{lanes.length > 1 ? lane.role : ''}</span>
                    <WaveCanvas
                        bins={lane.bins}
                        pcm={view.source?.lanes[index]?.getChannelData(0)}
                        {frames}
                        {start}
                        {end}
                        {markers}
                    />
                </div>
            {/each}
        </div>
        <button
            class="wave-seek"
            aria-label="Seek waveform"
            title="Seek waveform"
            onclick={(event) => {
                const rect = host!.getBoundingClientRect();
                view.cursor = Math.round(
                    clamp(start + ((event.clientX - rect.left) / rect.width) * (end - start), 0, frames - 1),
                );
                onseek(view.cursor);
            }}
            onkeydown={(event) => {
                if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
                    event.preventDefault();
                    view.cursor = clamp(
                        view.cursor + (event.key === 'ArrowLeft' ? -1 : 1) * (event.shiftKey ? 1000 : 1),
                        0,
                        frames - 1,
                    );
                    onseek(view.cursor);
                }
            }}
        ></button>
        {#each markers as frame, index}
            {#if Number.isFinite(frame) && position(frame) >= 0 && position(frame) <= 100}
                <button
                    class="wave-marker"
                    class:loop={index > 1}
                    class:end={index % 2 === 1}
                    class:label-left={(index % 2 === 1 && (position(frame) * width) / 100 >= 90) ||
                        ((100 - position(frame)) * width) / 100 < 90}
                    class:collision={index % 2 === 1 &&
                        ((position(frame) - position(markers[index - 1]!)) * width) / 100 < 90}
                    role="slider"
                    aria-label={names[index]}
                    aria-valuenow={frame}
                    aria-valuemin={bounds[index]![0]}
                    aria-valuemax={bounds[index]![1]}
                    aria-valuetext={`${Math.round(frame)} samples, ${((frame / rate) * 1000).toFixed(2)} milliseconds`}
                    title={`${names[index]}: ${Math.round(frame)} samples`}
                    disabled={blocked(index)}
                    style:left={`${position(frame)}%`}
                    onpointerdown={(event) => drag(event, index)}
                    onkeydown={(event) => {
                        const delta = event.shiftKey ? 1000 : 1;
                        if (event.key === 'ArrowLeft') change(index, frame - delta);
                        else if (event.key === 'ArrowRight') change(index, frame + delta);
                        else if (event.key === 'Home') change(index, bounds[index]![0]);
                        else if (event.key === 'End') change(index, bounds[index]![1]);
                        else return;
                        event.preventDefault();
                    }}><span>{labels[index]}</span></button
                >
            {/if}
        {/each}
        {#if position(view.cursor) >= 0 && position(view.cursor) <= 100}<div
                class="playhead"
                style:left={`${position(view.cursor)}%`}
            ></div>{/if}
    </div>
    <div class="wave-overview">
        <WaveCanvas bins={lanes[0]?.bins ?? []} {frames} overview />
        <span class="viewport-window" style:left={`${(start / frames) * 100}%`} style:width={`${100 / view.zoom}%`}
        ></span>
        <input
            type="range"
            aria-label="Pan zoomed waveform"
            min="0"
            max="1"
            step="0.001"
            value={view.pan}
            disabled={view.zoom === 1}
            oninput={(event) => (view.pan = Number(event.currentTarget.value))}
        />
    </div>
    <div class="marker-fields">
        {#each names as label, index}
            <div class="marker-field" class:loop={index > 1}>
                <AttributeHelp
                    {label}
                    description={index % 2
                        ? 'End boundary, in samples or milliseconds. Playback length is end minus start.'
                        : 'Start position in the stored Wave Data.'}
                />
                <div class="marker-value">
                    <EditorNumber
                        {label}
                        value={markers[index]}
                        min={bounds[index]![0]}
                        max={bounds[index]![1]}
                        scale={view.units ? rate / 1000 : 1}
                        unit={view.units ? 'ms' : ''}
                        disabled={blocked(index)}
                        resetValue={markerValues(
                            Object.fromEntries(keys.map((key) => [key, document.draft.baselineValue(key)!])),
                        )[index]}
                        onchange={(value) => change(index, value)}
                        onbegin={() => document.draft.beginGesture()}
                        onend={() => document.draft.endGesture()}
                        oninvalid={(message) => {
                            document.inputErrors = { ...document.inputErrors, [label]: message };
                        }}
                    />
                    {#if index > 1}<button
                            class="editor-icon catch"
                            aria-label={`Catch ${label.toLowerCase()}`}
                            title={`Catch playhead as ${label.toLowerCase()}`}
                            disabled={blocked(index)}
                            onclick={() => change(index, view.cursor, true)}
                            ><Icon name={index === 2 ? 'undo' : 'redo'} size={13} /></button
                        >{/if}
                </div>
            </div>
        {/each}
    </div>
    {#if view.loading || view.error}<div role="status" class="editor-meta">
            {view.loading ? 'Loading source audio' : view.error}
        </div>{/if}
</section>

<style>
    .waveform-page {
        display: flex;
        flex-direction: column;
        min-height: 192px;
        height: 100%;
        gap: 4px;
        min-width: 0;
    }
    .zoom {
        min-width: 20px;
        text-align: center;
    }
    .wave-ruler {
        display: flex;
        justify-content: space-between;
        color: var(--color-text-muted);
        font: 10px var(--font-mono, monospace);
        height: 16px;
        flex-shrink: 0;
    }
    .wave-surface {
        position: relative;
        flex: 1;
        min-height: 64px;
        background: var(--color-panel-deep);
        border-block: 1px solid var(--color-border);
        margin-inline: 2px;
        overflow: clip;
    }
    .wave-lanes {
        position: absolute;
        inset: 0;
        display: flex;
        flex-direction: column;
    }
    .wave-lane {
        flex: 1;
        min-height: 0;
        position: relative;
        background: repeating-linear-gradient(
            90deg,
            transparent 0,
            transparent calc(12.5% - 1px),
            #ffffff08 calc(12.5% - 1px),
            #ffffff08 12.5%
        );
    }
    .wave-lane + .wave-lane {
        border-top: 1px solid var(--color-border);
    }
    .lane-role {
        position: absolute;
        top: 5px;
        left: 8px;
        font-size: 9px;
        color: var(--color-text-muted);
        text-transform: uppercase;
    }
    .wave-seek {
        position: absolute;
        inset: 0;
        width: 100%;
        background: transparent;
        border: 0;
        cursor: crosshair;
    }
    .wave-marker {
        position: absolute;
        top: 0;
        bottom: 0;
        width: 2px;
        border: 0;
        padding: 0;
        background: var(--editor-trim);
        color: var(--color-panel-deep);
        touch-action: none;
        z-index: 2;
        cursor: ew-resize;
    }
    .wave-marker::before {
        content: '';
        position: absolute;
        inset: 0 -7px;
    }
    .wave-marker.end {
        transform: translateX(-2px);
    }
    .wave-marker > span {
        position: absolute;
        left: 0;
        bottom: 0;
        padding: 2px 4px;
        font: 600 10px var(--font-mono, monospace);
        white-space: nowrap;
        background: var(--editor-trim);
        border-radius: 2px;
    }
    .wave-marker.label-left > span {
        left: auto;
        right: 0;
    }
    .wave-marker.label-left:not(.end) > span {
        right: 2px;
    }
    .wave-marker.end:not(.label-left) > span {
        left: 2px;
    }
    .wave-marker.loop,
    .wave-marker.loop > span {
        background: var(--editor-loop);
    }
    .wave-marker.loop > span {
        bottom: auto;
        top: 0;
    }
    .wave-marker.collision > span {
        bottom: 20px;
    }
    .wave-marker.loop.collision > span {
        top: 20px;
        bottom: auto;
    }
    .playhead {
        position: absolute;
        inset-block: 0;
        width: 1px;
        background: #f1d49c;
        pointer-events: none;
    }
    .wave-overview {
        position: relative;
        height: 18px;
        flex: 0 0 18px;
        overflow: hidden;
        background: var(--color-panel-deep);
        border: 1px solid var(--color-border);
    }
    .viewport-window {
        position: absolute;
        inset-block: 0;
        border: 1px solid var(--color-accent);
        background: #7eafc818;
        pointer-events: none;
    }
    .wave-overview input {
        position: absolute;
        inset: 0;
        width: 100%;
        height: 100%;
        opacity: 0.02;
        margin: 0;
        cursor: ew-resize;
    }
    .wave-overview input:focus-visible {
        opacity: 1;
    }
    .marker-fields {
        display: grid;
        grid-template-columns: repeat(4, minmax(0, 1fr));
        gap: 12px;
    }
    .marker-field {
        min-width: 0;
        font-size: 10px;
        color: var(--editor-trim);
    }
    .marker-field.loop {
        color: var(--editor-loop);
    }
    .marker-value {
        display: flex;
        align-items: center;
        gap: 2px;
        margin-top: 2px;
    }
    .marker-value :global(.editor-number) {
        flex: 1;
    }
    .catch {
        width: 20px;
        flex-basis: 20px;
        color: var(--editor-loop);
    }
    :global([data-editor-under~='900']) .marker-fields {
        grid-template-columns: repeat(2, minmax(0, 1fr));
        gap: 6px 16px;
    }
    :global([data-editor-under~='900']) .waveform-page {
        min-height: 248px;
    }
    :global([data-editor-under~='500']) .wave-ruler .minor,
    :global([data-editor-under~='500']) .total {
        display: none;
    }
    :global([data-editor-under~='500']) .marker-value :global(.editor-number) {
        flex-direction: column;
        gap: 0;
        align-items: stretch;
    }
    :global([data-editor-under~='500']) .marker-value :global(.editor-value) {
        flex: 0 0 auto;
    }
    :global([data-editor-under~='500']) .waveform-page {
        min-height: 300px;
    }
</style>
