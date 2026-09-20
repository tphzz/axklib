<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import EditorChoice from '../../../object-editor/EditorChoice.svelte';
    import EditorNumber from '../../../object-editor/EditorNumber.svelte';
    import Icon from '../../../../lib/components/Icon.svelte';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import ParameterField from './ParameterField.svelte';
    import { sampleFields } from './fields';
    import { calculatedLoopTempo } from './sampleInfoGeometry';
    import { sampleView } from './view.svelte';
    import { parameterBlockReason } from './parameterAvailability';
    let {
        document,
        rate,
        disabled,
        onmonitor,
    }: {
        document: ObjectEditorDocument;
        rate: number;
        disabled: boolean;
        onmonitor: () => void;
    } = $props();
    const settings = $derived(sampleView(document).preferences);
    const fields = sampleFields.filter((field) =>
        ['loop_tempo_hundredths', 'wave_start_velocity_sensitivity'].includes(field.key),
    );
    const blockedTempo = $derived(
        disabled ||
            document.draft.values.loop_tempo_hundredths === undefined ||
            document.detail!.editing!.blockedParameters.includes('loop_tempo_hundredths'),
    );
    function calculate() {
        const result = calculatedLoopTempo(
            rate,
            Number(document.draft.values.loop_length_frames),
            settings.beats,
            settings.normalizeTempo,
        );
        document.status =
            result === undefined
                ? 'The selected loop and beat count do not give a tempo between 80.00 and 159.99 BPM'
                : '';
        if (result !== undefined) document.draft.set('loop_tempo_hundredths', result);
    }
</script>

<section class="sample-info" aria-label="Sample Info">
    <div class="editor-toolbar">
        <h3 class="editor-heading">Sample Info</h3>
        <span class="editor-spacer"></span>
        <span class="source-metadata editor-meta">
            <span>{rate.toLocaleString('en-US')} Hz</span><span aria-hidden="true">&middot;</span>
            <AttributeHelp
                label={`Source: ${(document.detail!.editing!.maximumFrames / rate).toFixed(3)} s`}
                description="Duration of the underlying source wave, independent of playback trimming and loop boundaries."
            />
        </span>
    </div>
    <div class="info-sections">
        <section>
            <h3 class="editor-heading">Sample parameters</h3>
            {#each fields as field}
                <ParameterField
                    {field}
                    draft={document.draft}
                    unavailableReason={document.detail?.editing?.unavailableParameters[field.key]?.message}
                    blockedReason={parameterBlockReason(field.key, document.detail!.editing!)}
                    disabled={disabled || document.detail!.editing!.blockedParameters.includes(field.key)}
                    oninvalid={(message) => (document.inputErrors = { ...document.inputErrors, [field.key]: message })}
                />
            {/each}
        </section>
        <section>
            <h3 class="editor-heading">
                <AttributeHelp
                    label="Display & audition"
                    description="Local editor preferences, retained while this image is open. End Type changes the end-boundary display, not the stored addresses. Loop Monitor affects preview only; neither writes the sampler's System settings."
                />
            </h3>
            <div class="parameter-field">
                <span class="field-label">End Type</span>
                <EditorChoice
                    label="End Type"
                    value={settings.endType}
                    options={['Address', 'Length', 'Time', 'Beat'].map((label, value) => ({ label, value }))}
                    onchange={(value) => (settings.endType = value)}
                />
            </div>
            <div class="monitor-field">
                <div class="parameter-field">
                    <span class="field-label"
                        ><AttributeHelp
                            label="Loop Monitor"
                            description="Starts preview up to 500 milliseconds before Loop Start, then repeats the loop. The lead-in plays once."
                        /></span
                    >
                    <EditorNumber
                        label="Loop monitor lead-in"
                        value={settings.monitorMs}
                        min={-500}
                        max={0}
                        unit="ms"
                        resetValue={-30}
                        onchange={(value) => (settings.monitorMs = value)}
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
        </section>
        <section>
            <h3 class="editor-heading">Tools</h3>
            <div class="parameter-field">
                <span class="field-label">Waveform ruler</span>
                <EditorChoice
                    label="Position display"
                    value={settings.units}
                    options={[
                        { value: 0, label: 'Samples' },
                        { value: 1, label: 'Time / ms' },
                    ]}
                    onchange={(value) => (settings.units = value)}
                />
            </div>
            <div class="parameter-field">
                <span class="field-label">Loop beats</span>
                <EditorChoice
                    label="Loop beat count"
                    value={settings.beats}
                    options={[1, 2, 4, 8].map((value) => ({ value, label: String(value) }))}
                    onchange={(value) => (settings.beats = value)}
                />
            </div>
            <div class="calculator">
                <label
                    ><input type="checkbox" bind:checked={settings.normalizeTempo} />
                    <AttributeHelp
                        label="Normalize tempo"
                        description="The hardware calculation assumes four beats, then doubles or halves the tempo to fit 80.00-159.99 BPM. Clear this to use the selected beat count literally."
                    /></label
                >
                <button
                    class="editor-icon"
                    aria-label="Calculate loop tempo"
                    title="Calculate and set Loop Tempo"
                    disabled={blockedTempo}
                    onclick={calculate}><Icon name="sparkles" size={14} /></button
                >
            </div>
        </section>
    </div>
</section>

<style>
    .sample-info {
        min-width: 0;
    }
    .info-sections {
        display: grid;
        grid-template-columns: repeat(3, minmax(0, 380px));
        gap: 12px;
    }
    .info-sections > section {
        min-width: 0;
    }
    h3 {
        margin: 0 0 6px;
    }
    .monitor-field,
    .calculator {
        display: flex;
        align-items: center;
        gap: 4px;
        min-width: 0;
    }
    .monitor-field :global(.parameter-field) {
        flex: 1;
    }
    .calculator {
        flex-wrap: wrap;
        justify-content: space-between;
    }
    .calculator label {
        display: inline-flex;
        align-items: center;
        gap: 4px;
        font-size: 10px;
    }
    .calculator input {
        accent-color: var(--color-accent);
    }
    .source-metadata {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        white-space: nowrap;
    }
    :global([data-editor-under~='1100']) .info-sections {
        grid-template-columns: repeat(2, minmax(0, 380px));
    }
    :global([data-editor-under~='650']) .info-sections {
        grid-template-columns: minmax(0, 1fr);
    }
</style>
