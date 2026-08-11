<script lang="ts">
    import type { AudioImportRow } from './audioImportDialogTypes';

    interface Props {
        row: AudioImportRow;
        panelId: string;
    }

    let { row, panelId }: Props = $props();
    const adjustments = $derived(row.inspection?.issues.filter((issue) => issue.fatal === false) ?? []);

    function sourceLabel(source: string): string {
        if (source === 'WAV_SMPL') return 'WAV sampler metadata (smpl chunk)';
        if (source === 'WAV_INST') return 'WAV instrument metadata (inst chunk)';
        return 'A-series default (no supported WAV value was applied)';
    }
</script>

{#if row.inspection}
    <div class="import-details" id={panelId} role="region" aria-label={`Import details for ${row.fileName}`}>
        <section>
            <h3>Initial value sources</h3>
            <dl>
                <div>
                    <dt>Pitch (root key and fine tune)</dt>
                    <dd>{sourceLabel(row.inspection.samplerDefaults.pitchSource)}</dd>
                </div>
                <div>
                    <dt>Key and velocity ranges</dt>
                    <dd>{sourceLabel(row.inspection.samplerDefaults.rangeSource)}</dd>
                </div>
                <div>
                    <dt>Playback and loop</dt>
                    <dd>{sourceLabel(row.inspection.samplerDefaults.loopSource)}</dd>
                </div>
            </dl>
        </section>
        {#if adjustments.length > 0}
            <section class="adjustments">
                <h3>Import adjustments</h3>
                <ul>
                    {#each adjustments as adjustment}
                        <li>{adjustment.message}</li>
                    {/each}
                </ul>
            </section>
        {/if}
    </div>
{/if}

<style>
    .import-details {
        display: grid;
        grid-template-columns: minmax(0, 2fr) minmax(220px, 1fr);
        gap: 14px 22px;
        padding-top: 10px;
        border-top: 1px solid var(--color-border);
        color: var(--color-text-muted);
        font-size: 11px;
    }
    section {
        min-width: 0;
    }
    h3 {
        margin: 0 0 7px;
        color: var(--color-text-strong);
        font-size: 11px;
        font-weight: 600;
    }
    dl {
        display: grid;
        grid-template-columns: repeat(3, minmax(0, 1fr));
        gap: 8px 16px;
        margin: 0;
    }
    dl div {
        min-width: 0;
    }
    dt {
        color: var(--color-text);
    }
    dd {
        margin: 2px 0 0;
        line-height: 1.4;
    }
    ul {
        display: grid;
        gap: 4px;
        margin: 0;
        padding-left: 16px;
        color: var(--color-warning);
        line-height: 1.4;
    }
    @media (max-width: 900px) {
        .import-details,
        dl {
            grid-template-columns: minmax(0, 1fr);
        }
    }
</style>
