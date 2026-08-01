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

    function sourceLabel(source: string): string {
        if (source === 'WAV_SMPL') return 'WAV smpl';
        if (source === 'WAV_INST') return 'WAV inst';
        return 'A-series default';
    }
</script>

<div class="sampler-settings" aria-label={`Sampler settings for ${row.fileName}`}>
    <div class="settings-fields">
        <label>
            <span>Root key</span>
            <span class="note-input">
                <input
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
        <label>
            <span>Playback</span>
            <select
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
                <option value="4">One-shot</option>
                <option value="1">Forward loop</option>
            </select>
        </label>
        {#if row.loopMode === 1}
            <label>
                <span>Loop start</span>
                <input
                    aria-label={`Loop start for ${row.fileName}`}
                    type="number"
                    min="0"
                    max="4294967295"
                    value={row.loopStartFrame}
                    {disabled}
                    oninput={(event) => onupdate(row.id, { loopStartFrame: numberValue(event) })}
                />
            </label>
            <label>
                <span>Loop length</span>
                <input
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
    {#if row.inspection}
        <div class="settings-sources">
            <span>Pitch: {sourceLabel(row.inspection.samplerDefaults.pitchSource)}</span>
            <span>Ranges: {sourceLabel(row.inspection.samplerDefaults.rangeSource)}</span>
            <span>Playback: {sourceLabel(row.inspection.samplerDefaults.loopSource)}</span>
        </div>
        {#each row.inspection.issues.filter((issue) => issue.fatal === false) as issue}
            <p class="settings-warning" role="status">{issue.message}</p>
        {/each}
    {/if}
</div>

<style>
    .sampler-settings {
        display: grid;
        gap: 9px;
        padding: 2px 0;
    }
    .settings-fields {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(130px, 1fr));
        gap: 9px 12px;
    }
    label {
        display: grid;
        gap: 4px;
        color: var(--color-text-muted);
        font-size: 11px;
    }
    input,
    select {
        width: 100%;
        min-width: 0;
        height: var(--density-control);
        padding: 0 8px;
        color: var(--color-text-strong);
        border: 1px solid var(--color-border);
        border-radius: 5px;
        outline: none;
        background: var(--color-bg-deep);
        font-size: 12px;
    }
    input:focus,
    select:focus {
        border-color: var(--color-accent);
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
        font-size: 11px;
        white-space: nowrap;
    }
    .settings-sources {
        display: flex;
        flex-wrap: wrap;
        gap: 6px 18px;
        color: var(--color-text-muted);
        font-size: 11px;
    }
    .settings-warning {
        margin: 0;
        color: var(--color-warning);
        font-size: 11px;
        line-height: 1.4;
    }
</style>
