<script lang="ts">
    import { onDestroy, untrack, tick } from 'svelte';
    import { on } from 'svelte/events';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { EditorValues } from '../../../object-editor/draft.svelte';
    import { editorAudio } from '../../../object-editor/audioContext';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import Icon from '../../../../lib/components/Icon.svelte';
    import { loadEditorAudio } from '../../../object-editor/audioSource';
    import { draftPlaybackPlan, prepareSampleDraft } from './audition';
    import { sampleView } from './view.svelte';
    import { clamp, noteName, sourceFrame } from './geometry';
    import { userFacingMessage } from '../../../../lib/userFacingMessage';
    let { document, rate, disabled }: { document: ObjectEditorDocument; rate: number; disabled: boolean } = $props();
    const audio = editorAudio();
    const view = $derived(sampleView(document));
    $effect(() => {
        document.detail;
        untrack(() => {
            if (playing || busy) void audio?.audition.stop();
            view.release();
        });
    });
    let run = $state.raw<{
        values: EditorValues;
        start: number;
        length: number;
        speed: number;
        outputRate: number;
        sourceRate: number;
    }>();
    const playing = $derived(
        !!audio?.audition.state?.draft &&
            audio.audition.state.objectId === document.detail!.object.id &&
            audio.audition.state.status === 'playing',
    );
    const busy = $derived(
        audio?.audition.state?.objectId === document.detail!.object.id && audio.audition.state.status === 'preparing',
    );
    const status = $derived(
        document.validation ||
            document.status ||
            (audio?.audition.state?.objectId === document.detail!.object.id ? audio.audition.state.error : '') ||
            (playing ? 'Playing' : busy ? 'Preparing audio' : 'Ready'),
    );
    $effect(() => {
        if (playing && run)
            view.cursor = sourceFrame(
                audio!.audition.state.playheadFrame,
                run.outputRate,
                run.sourceRate,
                run.speed,
                run.start,
                run.length,
                false,
            );
    });
    $effect(() => {
        if (view.gain) view.gain.gain.value = view.volume / 100;
    });
    $effect(() => {
        if (run && (disabled || document.draft.values !== run.values)) {
            run = undefined;
            if (playing || busy) void audio?.audition.stop();
        }
    });
    $effect(() => {
        const release = on(window, 'keydown', (event) => {
            if (
                (event.target as HTMLElement)?.closest(
                    'input,select,textarea,button,[role=slider],[role=listbox],[role=dialog]',
                )
            )
                return;
            if (event.code === 'Space') {
                event.preventDefault();
                if (playing || busy) void audio?.audition.stop();
                else void play();
            } else if (event.key === 'Escape' && (playing || busy)) {
                event.preventDefault();
                void audio?.audition.stop();
            }
        });
        return release;
    });
    function mode(next: number) {
        const changes: EditorValues = { loop_mode: next };
        if ([1, 2].includes(next) && Number(document.draft.values.loop_length_frames) === 0) {
            changes.loop_start_frame = document.draft.values['playback.start_frame']!;
            changes.loop_length_frames = document.draft.values['playback.length_frames']!;
        }
        document.draft.patch(changes);
    }
    export async function play(monitor = false) {
        if (!audio || disabled || document.validation) return;
        const identity = document.detail!;
        const original = document.draft.values;
        const values = { ...original };
        if (monitor) {
            const start = Math.max(0, Number(values.loop_start_frame) + Math.round((view.monitorMs / 1000) * rate));
            values['playback.start_frame'] = start;
            values['playback.length_frames'] =
                Number(values.loop_start_frame) + Number(values.loop_length_frames) - start;
            values.loop_mode = 1;
        }
        const cursor = view.cursor;
        try {
            await audio.audition.playPrepared(document.sessionId, identity.object.id, async (context, signal) => {
                const source =
                    view.source ??
                    (await loadEditorAudio(audio.transport, document.sessionId, identity.object.id, context, signal));
                signal.throwIfAborted();
                view.source = source;
                view.gain?.disconnect();
                view.gain = context.createGain();
                view.gain.gain.value = view.volume / 100;
                view.gain.connect(context.destination);
                const plan = draftPlaybackPlan(values, source.sampleRate, context.sampleRate, view.note);
                run = {
                    values: original,
                    start: plan.start,
                    length: plan.length,
                    speed: plan.speed,
                    outputRate: context.sampleRate,
                    sourceRate: source.sampleRate,
                };
                return prepareSampleDraft(
                    audio.transport,
                    document.sessionId,
                    identity.object.id,
                    identity.editing!,
                    values,
                    view.note,
                    context,
                    signal,
                    { source, output: view.gain },
                );
            });
            await tick();
            if (!monitor && playing && run && cursor > run.start && cursor < run.start + run.length) seek(cursor);
        } catch (error) {
            document.status = userFacingMessage(error);
        }
    }
    export function seek(frame: number) {
        if (playing && run)
            audio?.audition.seekPrepared(
                Math.round(((frame - run.start) * run.outputRate) / run.sourceRate / run.speed),
            );
    }
    onDestroy(() => {
        if (playing || busy) void audio?.audition.stop();
        view.release();
    });
</script>

<footer class="sample-transport">
    <button
        class="audition-button"
        aria-label={playing || busy ? 'Stop draft preview' : 'Play draft'}
        disabled={!playing && !busy && (disabled || !!document.validation)}
        onclick={() => (playing || busy ? void audio?.audition.stop() : void play())}
        ><Icon name={playing || busy ? 'stop' : 'play'} size={13} /><span>{playing || busy ? 'Stop' : 'Audition'}</span
        ></button
    >
    <button
        class="editor-icon"
        aria-label="Return to wave start"
        title="Return to wave start"
        onclick={() => {
            view.cursor = Number(document.draft.values['playback.start_frame']);
            seek(view.cursor);
        }}><Icon name="undo" size={13} /></button
    >
    <span class="playhead-readout">{(view.cursor / rate).toFixed(3)} <small>s</small></span>
    <div class="playback-modes">
        <EditorChoice
            label="Playback"
            segmented={true}
            value={Number(document.draft.values.loop_mode)}
            disabled={disabled ||
                ['loop_mode', 'loop_start_frame', 'loop_length_frames'].some((key) =>
                    document.detail!.editing!.blockedParameters.includes(key),
                )}
            options={[
                { value: 0, label: 'Forward' },
                { value: 1, label: 'Loop' },
                { value: 2, label: 'Until release' },
                { value: 3, label: 'Reverse' },
                { value: 4, label: 'One-shot fwd' },
                { value: 5, label: 'One-shot rev' },
            ]}
            onchange={mode}
        />
    </div>
    <div class="preview-note">
        <input
            class="editor-control"
            aria-label="Preview MIDI note"
            type="number"
            min="0"
            max="127"
            value={view.note}
            onchange={(event) => {
                const value = event.currentTarget.valueAsNumber;
                if (Number.isFinite(value)) {
                    void audio?.audition.stop();
                    view.note = Math.round(clamp(value, 0, 127));
                }
            }}
        /><span class="editor-meta">{noteName(view.note)}</span>
    </div>
    <div class="monitor-volume">
        <span class="editor-meta">Vol</span><input
            class="editor-slider"
            type="range"
            aria-label="Audition volume"
            min="0"
            max="100"
            step="1"
            bind:value={view.volume}
        />
    </div>
    {#if audio}<label class="autoplay"><input type="checkbox" bind:checked={audio.audition.autoplay} />Autoplay</label
        >{/if}
    <span class="loop-readout editor-meta"
        >Loop {(Number(document.draft.values.loop_length_frames) / rate).toFixed(3)} s</span
    >
    <span class="transport-status" role="status" title={status}>{status}</span>
    <AttributeHelp
        label="Draft audition"
        description={'Preview applies playback and loop bounds, pitch, level and pan.\n\nFilter, envelopes, LFO, routing and effects are stored for sampler playback. Audition volume and loop monitoring are local preview settings.'}
        ><Icon name="info" size={13} /></AttributeHelp
    >
</footer>

<style>
    .sample-transport {
        display: flex;
        align-items: center;
        gap: 6px;
        padding: 5px 8px;
        flex: 0 0 auto;
        min-height: 36px;
        border-top: 1px solid var(--color-border);
        background: var(--color-panel-raised);
    }
    .audition-button {
        display: inline-flex;
        align-items: center;
        gap: 5px;
        height: 26px;
        width: 82px;
        flex: 0 0 82px;
        justify-content: center;
        padding: 0 8px;
        border: 1px solid var(--color-accent);
        border-radius: 3px;
        background: var(--color-accent);
        color: var(--color-panel-deep);
        font-size: 11px;
        font-weight: 600;
    }
    .playhead-readout {
        font: 11px var(--font-mono, monospace);
        width: 64px;
        flex-shrink: 0;
        text-align: right;
        white-space: nowrap;
    }
    small {
        color: var(--color-text-muted);
        font-size: 10px;
    }
    .playback-modes {
        flex: 0 0 auto;
        min-width: 0;
    }
    .playback-modes :global(.editor-choice) {
        flex: 0 0 auto;
        white-space: nowrap;
        font-size: 10px;
        padding-inline: 6px;
    }
    .preview-note {
        display: flex;
        align-items: center;
        gap: 4px;
        flex-shrink: 0;
    }
    .preview-note input {
        width: 44px;
    }
    .monitor-volume {
        display: flex;
        align-items: center;
        width: 84px;
        gap: 4px;
        flex-shrink: 0;
    }
    .autoplay {
        display: flex;
        align-items: center;
        gap: 4px;
        font-size: 10px;
        color: var(--color-text-muted);
        white-space: nowrap;
        flex-shrink: 0;
    }
    .autoplay input {
        accent-color: var(--color-accent);
        width: 12px;
        height: 12px;
        margin: 0;
    }
    .transport-status {
        flex: 1;
        min-width: 30px;
        text-align: right;
        color: var(--color-text-muted);
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        font-size: 10px;
        line-height: 18px;
    }
    .loop-readout {
        flex: 0 0 auto;
        white-space: nowrap;
    }
    :global([data-editor-under~='500']) .audition-button {
        width: 30px;
        flex-basis: 30px;
    }
    :global([data-editor-under~='1200']) .loop-readout {
        display: none;
    }
    :global([data-editor-under~='900']) .sample-transport {
        flex-wrap: wrap;
    }
    :global([data-editor-under~='900']) .playback-modes {
        order: 2;
        flex-basis: 100%;
    }
    :global([data-editor-under~='900']) .monitor-volume {
        width: 65px;
    }
    :global([data-editor-under~='500']) .audition-button > span,
    :global([data-editor-under~='500']) .autoplay,
    :global([data-editor-under~='500']) .monitor-volume {
        display: none;
    }
    :global([data-editor-under~='500']) .sample-transport {
        gap: 4px;
    }
    :global([data-editor-under~='500']) .playback-modes :global(.editor-choice) {
        flex: 1;
        white-space: normal;
        line-height: 13px;
        height: 30px;
        padding: 2px 3px;
    }
</style>
