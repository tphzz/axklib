<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import EditorNumber from '../../../object-editor/EditorNumber.svelte';
    import Icon from '../../../../lib/components/Icon.svelte';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import ParameterField from './ParameterField.svelte';
    import { sampleFields } from './fields';
    import { calculateTempo } from './geometry';
    import { sampleView } from './view.svelte';
    let {
        document,
        rate,
        disabled,
        onmonitor,
    }: { document: ObjectEditorDocument; rate: number; disabled: boolean; onmonitor: () => void } = $props();
    const view = $derived(sampleView(document));
    const tempo = sampleFields.find((field) => field.key === 'loop_tempo_hundredths')!;
    const velocity = sampleFields.find((field) => field.key === 'wave_start_velocity_sensitivity')!;
    function calculate() {
        const result = calculateTempo(rate, Number(document.draft.values.loop_length_frames), view.beats);
        document.status = result === null ? 'Choose a beat count that gives a tempo between 80.00 and 159.99 BPM' : '';
        if (result !== null) document.draft.set('loop_tempo_hundredths', result);
    }
    const blockedTempo = $derived(
        disabled ||
            document.draft.values.loop_tempo_hundredths === undefined ||
            document.detail!.editing!.blockedParameters.includes('loop_tempo_hundredths'),
    );
</script>

<section class="settings-page" aria-label="Sample settings">
    <div class="editor-toolbar">
        <h3 class="editor-heading">Sample</h3>
        <span class="editor-spacer"></span>
        <span class="source-metadata editor-meta">
            <span>{rate.toLocaleString('en-US')} Hz</span><span aria-hidden="true">&middot;</span>
            <AttributeHelp
                label={`Source: ${(document.detail!.editing!.maximumFrames / rate).toFixed(3)} s`}
                description="Duration of the underlying source wave, independent of playback trimming and loop boundaries."
            />
        </span>
    </div>
    <div class="settings-grid">
        <div class="parameter-field">
            <span class="field-label">Position</span><EditorChoice
                label="Position display"
                value={view.units}
                options={[
                    { value: 0, label: 'Samples' },
                    { value: 1, label: 'Time / ms' },
                ]}
                onchange={(next) => (view.units = next)}
            />
        </div>
        <div class="tempo-field">
            <ParameterField
                field={tempo}
                draft={document.draft}
                unavailableReason={document.detail?.editing?.unavailableParameters[tempo.key]?.message}
                disabled={blockedTempo}
                oninvalid={(message) => (document.inputErrors = { ...document.inputErrors, tempo: message })}
            />
            <button
                class="editor-icon"
                aria-label="Calculate loop tempo"
                title="Calculate tempo from loop length and beat count"
                disabled={blockedTempo}
                onclick={calculate}><Icon name="sparkles" size={14} /></button
            >
        </div>
        <div class="parameter-field">
            <span class="field-label">Loop beats</span><EditorChoice
                label="Loop beat count"
                value={view.beats}
                options={[1, 2, 4, 8].map((value) => ({ value, label: String(value) }))}
                onchange={(next) => (view.beats = next)}
            />
        </div>
        <div class="monitor-field">
            <div class="parameter-field">
                <span class="field-label"
                    ><AttributeHelp
                        label="Loop monitor"
                        description="Starts playback this many milliseconds before the loop start, then repeats the loop. The lead-in plays once."
                    /></span
                >
                <EditorNumber
                    label="Loop monitor lead-in"
                    value={view.monitorMs}
                    min={-500}
                    max={0}
                    step={1}
                    unit="ms"
                    resetValue={-30}
                    onchange={(next) => (view.monitorMs = next)}
                />
            </div>
            <button
                class="editor-icon"
                aria-label="Monitor loop"
                title="Monitor loop with lead-in"
                disabled={disabled || Number(document.draft.values.loop_length_frames) <= 0}
                onclick={onmonitor}><Icon name="play" size={14} /></button
            >
        </div>
        <ParameterField
            field={velocity}
            draft={document.draft}
            unavailableReason={document.detail?.editing?.unavailableParameters[velocity.key]?.message}
            disabled={disabled || document.detail!.editing!.blockedParameters.includes(velocity.key)}
            oninvalid={(message) => (document.inputErrors = { ...document.inputErrors, velocity: message })}
        />
    </div>
</section>

<style>
    .settings-page {
        height: 100%;
        min-height: 160px;
        display: flex;
        flex-direction: column;
        gap: 8px;
    }
    .settings-grid {
        display: grid;
        grid-template-columns: repeat(3, minmax(0, 1fr));
        gap: 16px 24px;
    }
    .tempo-field,
    .monitor-field {
        display: flex;
        gap: 4px;
        align-items: center;
        min-width: 0;
    }
    .tempo-field :global(.parameter-field),
    .monitor-field :global(.parameter-field) {
        flex: 1;
    }
    .source-metadata {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        white-space: nowrap;
    }
    :global([data-editor-under~='1100']) .settings-grid {
        grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    :global([data-editor-under~='650']) .settings-grid {
        grid-template-columns: minmax(0, 1fr);
        gap: 8px;
    }
</style>
