<script lang="ts">
    import { noteName } from '../audioImport';
    import type { AudioImportRow } from './audioImportDialogTypes';

    interface Props {
        row: AudioImportRow;
        disabled: boolean;
        onupdate: (id: number, update: Partial<AudioImportRow>) => void;
    }

    let { row, disabled, onupdate }: Props = $props();

    function numberValue(event: Event): number {
        return (event.currentTarget as HTMLInputElement).valueAsNumber;
    }
</script>

<div class="sampler-settings" aria-label={`Sampler settings for ${row.fileName}`}>
    <div class="settings-fields">
        <label>
            <span>Root key</span>
            <span class="note-input">
                <input
                    class="dialog-field-control"
                    aria-label={`Root key for ${row.fileName}`}
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
        <label>
            <span>Fine tune</span>
            <span class="unit-input">
                <input
                    class="dialog-field-control"
                    aria-label={`Fine tune for ${row.fileName}`}
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
        <label>
            <span>Key range</span>
            <span class="range-inputs">
                <input
                    class="dialog-field-control"
                    aria-label={`Lowest key for ${row.fileName}`}
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
                    type="number"
                    min="0"
                    max="127"
                    value={row.keyHigh}
                    {disabled}
                    oninput={(event) => onupdate(row.id, { keyHigh: numberValue(event) })}
                />
            </span>
        </label>
        <label>
            <span>Velocity range</span>
            <span class="range-inputs">
                <input
                    class="dialog-field-control"
                    aria-label={`Lowest velocity for ${row.fileName}`}
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
            <select
                class="dialog-field-control"
                aria-label={`Playback mode for ${row.fileName}`}
                value={row.loopMode}
                {disabled}
                onchange={(event) => {
                    const loopMode = Number((event.currentTarget as HTMLSelectElement).value) as 1 | 4;
                    onupdate(row.id, {
                        loopMode,
                        ...(loopMode === 4 ? { loopStartFrame: 0, loopLengthFrames: 0 } : {}),
                    });
                }}
            >
                <option value={4}>One-shot</option>
                <option value={1}>Forward loop</option>
            </select>
        </label>
        {#if row.loopMode === 1}
            <label class="frame-setting">
                <span>Loop start</span>
                <input
                    class="dialog-field-control"
                    aria-label={`Loop start for ${row.fileName}`}
                    type="number"
                    min="0"
                    max="4294967295"
                    value={row.loopStartFrame}
                    {disabled}
                    oninput={(event) => onupdate(row.id, { loopStartFrame: numberValue(event) })}
                />
            </label>
            <label class="frame-setting">
                <span>Loop length</span>
                <input
                    class="dialog-field-control"
                    aria-label={`Loop length for ${row.fileName}`}
                    type="number"
                    min="1"
                    value={row.loopLengthFrames}
                    {disabled}
                    oninput={(event) => onupdate(row.id, { loopLengthFrames: numberValue(event) })}
                />
            </label>
        {/if}
    </div>
</div>

<style>
    .sampler-settings {
        display: grid;
        gap: 6px;
    }
    .settings-fields {
        display: flex;
        flex-wrap: wrap;
        justify-content: flex-start;
        align-items: flex-end;
        gap: 6px 10px;
    }
    .settings-fields > label {
        flex: 0 0 auto;
    }
    label {
        display: grid;
        gap: 4px;
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }
    input,
    select {
        width: 100%;
        min-width: 0;
    }
    .note-input,
    .unit-input,
    .range-inputs {
        display: flex;
        align-items: center;
        gap: 6px;
    }
    .note-input input,
    .unit-input input,
    .range-inputs input {
        width: 62px;
        flex: none;
    }
    .note-input small,
    .unit-input small,
    .range-inputs span {
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
        white-space: nowrap;
    }
    .playback-setting {
        width: 180px;
        max-width: 100%;
    }
    .frame-setting {
        width: 140px;
        max-width: 100%;
    }
</style>
