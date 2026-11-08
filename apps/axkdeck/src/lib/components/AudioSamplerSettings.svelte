<script lang="ts">
    import { noteName } from '../audioImport';
    import type { AudioImportRow, AudioImportValidation } from './audioImportDialogTypes';
    import SelectControl from './SelectControl.svelte';

    interface Props {
        row: AudioImportRow;
        disabled: boolean;
        invalidField?: AudioImportValidation['field'];
        errorId?: string;
        onupdate: (id: number, update: Partial<AudioImportRow>) => void;
    }
    let { row, disabled, onupdate, invalidField, errorId }: Props = $props();
    function numberValue(event: Event): number {
        return (event.currentTarget as HTMLInputElement).valueAsNumber;
    }
</script>

<div class="sampler-settings" role="group" aria-label={`Sampler settings for ${row.fileName}`}>
    <div class="settings-fields">
        <div class="sampler-pitch">
            <label class="root-setting">
                <span><span class="wide-label">Root key</span><span class="short-label">Root</span></span>
                <span class="note-input">
                    <input
                        class="dialog-field-control"
                        aria-label={`Root key for ${row.fileName}`}
                        aria-invalid={invalidField === 'rootKey' || undefined}
                        aria-describedby={invalidField === 'rootKey' ? errorId : undefined}
                        type="number"
                        min="0"
                        max="127"
                        value={row.rootKey}
                        {disabled}
                        oninput={(event) => onupdate(row.id, { rootKey: numberValue(event) })}
                    />
                    <small>{noteName(row.rootKey)}</small>
                </span>
            </label>
            <label class="tune-setting">
                <span>Fine tune</span>
                <span class="unit-input">
                    <input
                        class="dialog-field-control"
                        aria-label={`Fine tune for ${row.fileName}`}
                        aria-invalid={invalidField === 'fineTuneCents' || undefined}
                        aria-describedby={invalidField === 'fineTuneCents' ? errorId : undefined}
                        type="number"
                        min="-63"
                        max="63"
                        value={row.fineTuneCents}
                        {disabled}
                        oninput={(event) => onupdate(row.id, { fineTuneCents: numberValue(event) })}
                    />
                    <small>cent</small>
                </span>
            </label>
        </div>
        <label class="key-setting">
            <span>Key range</span>
            <span class="range-inputs">
                <input
                    class="dialog-field-control"
                    aria-label={`Lowest key for ${row.fileName}`}
                    aria-invalid={invalidField === 'keyRange' || undefined}
                    aria-describedby={invalidField === 'keyRange' ? errorId : undefined}
                    type="number"
                    min="0"
                    max="127"
                    value={row.keyLow}
                    {disabled}
                    oninput={(event) => onupdate(row.id, { keyLow: numberValue(event) })}
                />
                <span>to</span>
                <input
                    class="dialog-field-control"
                    aria-label={`Highest key for ${row.fileName}`}
                    aria-invalid={invalidField === 'keyRange' || undefined}
                    aria-describedby={invalidField === 'keyRange' ? errorId : undefined}
                    type="number"
                    min="0"
                    max="127"
                    value={row.keyHigh}
                    {disabled}
                    oninput={(event) => onupdate(row.id, { keyHigh: numberValue(event) })}
                />
            </span>
        </label>
        <label class="velocity-setting">
            <span><span class="wide-label">Velocity range</span><span class="short-label">Velocity</span></span>
            <span class="range-inputs">
                <input
                    class="dialog-field-control"
                    aria-label={`Lowest velocity for ${row.fileName}`}
                    aria-invalid={invalidField === 'velocityRange' || undefined}
                    aria-describedby={invalidField === 'velocityRange' ? errorId : undefined}
                    type="number"
                    min="0"
                    max="127"
                    value={row.velocityLow}
                    {disabled}
                    oninput={(event) => onupdate(row.id, { velocityLow: numberValue(event) })}
                />
                <span>to</span>
                <input
                    class="dialog-field-control"
                    aria-label={`Highest velocity for ${row.fileName}`}
                    aria-invalid={invalidField === 'velocityRange' || undefined}
                    aria-describedby={invalidField === 'velocityRange' ? errorId : undefined}
                    type="number"
                    min="0"
                    max="127"
                    value={row.velocityHigh}
                    {disabled}
                    oninput={(event) => onupdate(row.id, { velocityHigh: numberValue(event) })}
                />
            </span>
        </label>
        <label class="playback-setting">
            <span>Playback</span>
            <SelectControl
                label={`Playback mode for ${row.fileName}`}
                invalid={invalidField === 'loop'}
                describedBy={invalidField === 'loop' ? errorId : undefined}
                value={row.loopMode}
                options={[
                    { value: 4, label: 'One-shot' },
                    { value: 1, label: 'Forward loop' },
                ] as const}
                {disabled}
                onchange={(loopMode) =>
                    onupdate(row.id, {
                        loopMode,
                        ...(loopMode === 4 ? { loopStartFrame: 0, loopLengthFrames: 0 } : {}),
                    })}
            />
        </label>
        {#if row.loopMode === 1}
            <div class="loop-settings">
                <label class="frame-setting">
                    <span>Loop start (frames)</span>
                    <input
                        class="dialog-field-control"
                        aria-label={`Loop start for ${row.fileName}`}
                        aria-invalid={invalidField === 'loop' || undefined}
                        aria-describedby={invalidField === 'loop' ? errorId : undefined}
                        type="number"
                        min="0"
                        max="4294967295"
                        value={row.loopStartFrame}
                        {disabled}
                        oninput={(event) => onupdate(row.id, { loopStartFrame: numberValue(event) })}
                    />
                </label>
                <label class="frame-setting">
                    <span>Loop length (frames)</span>
                    <input
                        class="dialog-field-control"
                        aria-label={`Loop length for ${row.fileName}`}
                        aria-invalid={invalidField === 'loop' || undefined}
                        aria-describedby={invalidField === 'loop' ? errorId : undefined}
                        type="number"
                        min="1"
                        value={row.loopLengthFrames}
                        {disabled}
                        oninput={(event) => onupdate(row.id, { loopLengthFrames: numberValue(event) })}
                    />
                </label>
            </div>
        {/if}
    </div>
</div>

<style>
    .sampler-settings {
        grid-column: 1 / -1;
        min-width: 0;
    }
    .settings-fields {
        display: grid;
        grid-template-columns: var(--audio-import-parameter-columns);
        align-items: start;
        gap: 4px;
    }
    .sampler-pitch,
    label,
    .note-input,
    .unit-input,
    .range-inputs {
        display: flex;
        align-items: center;
        gap: 4px;
        min-width: 0;
    }
    label {
        color: var(--color-text);
        font-size: var(--dialog-label-font-size);
        line-height: 14px;
    }
    label > span:first-child {
        flex: none;
        white-space: nowrap;
    }
    .root-setting > span:first-child {
        width: 22px;
    }
    .tune-setting > span:first-child {
        width: 44px;
    }
    .key-setting > span:first-child {
        width: 48px;
    }
    .velocity-setting > span:first-child {
        width: 38px;
    }
    .playback-setting > span:first-child {
        width: 43px;
    }
    .wide-label {
        display: none;
    }
    .note-input input,
    .unit-input input,
    .range-inputs input {
        width: 62px;
        min-width: 62px;
        height: 26px;
        flex: none;
    }
    .note-input small {
        width: 28px;
        flex: none;
    }
    .unit-input small {
        width: 21px;
        flex: none;
    }
    small,
    .range-inputs > span {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
    .range-inputs > span {
        width: 10px;
        flex: none;
    }
    .playback-setting > :global(.select-control) {
        flex: 1;
        min-width: 0;
    }
    .loop-settings {
        display: grid;
        grid-column: 1 / -1;
        grid-template-columns: var(--audio-import-parameter-columns);
        gap: 4px;
    }
    .frame-setting {
        display: grid;
        gap: 2px;
    }
    .frame-setting input {
        width: 100%;
        min-width: 0;
    }
    input[aria-invalid='true'] {
        border-color: var(--color-danger);
    }
    @container audio-import-rows (min-width: 1230px) {
        .sampler-settings {
            grid-column: 2 / -1;
            grid-row: 2;
        }
        .root-setting > span:first-child {
            width: 42px;
        }
        .velocity-setting > span:first-child {
            width: 68px;
        }
        .wide-label {
            display: inline;
        }
        .short-label {
            display: none;
        }
    }
    @container audio-import-rows (max-width: 813px) {
        .settings-fields {
            grid-template-columns: repeat(2, minmax(0, 1fr));
            gap: 6px;
        }
        .sampler-pitch {
            grid-column: 1 / -1;
            flex-wrap: wrap;
        }
        .loop-settings {
            grid-template-columns: repeat(2, minmax(0, 1fr));
        }
        .playback-setting {
            grid-column: 1 / -1;
        }
    }
    @container audio-import-rows (max-width: 465px) {
        .settings-fields,
        .loop-settings {
            grid-template-columns: minmax(0, 1fr);
        }
        .sampler-pitch {
            gap: 6px 12px;
        }
        .key-setting,
        .velocity-setting {
            justify-content: space-between;
        }
    }
    @container audio-import-rows (max-width: 300px) {
        .key-setting,
        .velocity-setting {
            display: grid;
            justify-content: start;
            gap: 2px;
        }
    }
</style>
