<script lang="ts">
    import type { components } from '../../../lib/generated/axklibApiV1';
    import type { OpenedImage } from '../../../lib/transport';
    import InspectorModeFooter from '../../../lib/components/InspectorModeFooter.svelte';
    let {
        detail,
        relationships,
        objects,
        sources,
        onselect,
        onrevealFiles,
    }: {
        detail: components['schemas']['Tx16wObjectDetail'] | null;
        relationships: components['schemas']['ImageRelationshipItem'][];
        objects: components['schemas']['ImageObjectItem'][];
        sources: OpenedImage['sources'];
        onselect: (id: string) => void;
        onrevealFiles: (id: string) => void;
    } = $props();
    const labels: Record<string, string> = {
        sampleRate: 'Sample rate',
        attackFrames: 'Attack frames',
        repeatFrames: 'Repeat frames',
        looped: 'Looped',
        slot: 'Slot',
        waveSlot: 'Wave slot',
        rootKey: 'Root key',
        writeProtected: 'Write protected',
        kind: 'Native kind',
    };
    function objectName(id: string) {
        return objects.find((object) => object.id === id)?.name || id;
    }
    function sourceName(id: string) {
        const source = sources.find((item) => item.id === id)?.source;
        return !source
            ? id
            : source.kind === 'FILE'
              ? source.file.relativePath
              : source.kind === 'DIRECTORY'
                ? source.directory.relativePath
                : id;
    }
</script>

<aside class="tx-inspector" aria-label="TX16W inspector">
    <header>
        <h2>{detail?.name || 'Object details'}</h2>
        <span>{detail?.type ?? ''}</span>
    </header>
    <div class="detail-scroll">
        {#if detail}
            <dl>
                {#each Object.entries(detail.details).filter(([key, value]) => key !== 'type' && !Array.isArray(value)) as [key, value] (key)}
                    <div>
                        <dt>{labels[key] ?? key}</dt>
                        <dd>{typeof value === 'boolean' ? (value ? 'Yes' : 'No') : value}</dd>
                    </div>
                {/each}
            </dl>
            {#if detail.details.type === 'voice'}
                <h3>Key regions</h3>
                <table>
                    <thead><tr><th>Timbre</th><th>Keys</th><th>Fade</th></tr></thead><tbody>
                        {#each detail.details.regions as region}<tr
                                ><td>{region.timbreSlot}</td><td>{region.lowKey}-{region.highKey}</td><td
                                    >{region.fade}</td
                                ></tr
                            >{/each}
                    </tbody>
                </table>
            {:else if detail.details.type === 'performance'}
                <h3>Voice assignments</h3>
                {#each detail.details.voices as voice, index}
                    <h4>Part {index + 1}</h4>
                    <dl>
                        <div>
                            <dt>Voice slot</dt>
                            <dd>{voice.voiceSlot}</dd>
                        </div>
                        <div>
                            <dt>Receive channel</dt>
                            <dd>{voice.receiveChannel}</dd>
                        </div>
                        <div>
                            <dt>Alternative group</dt>
                            <dd>{voice.alternativeGroup}</dd>
                        </div>
                        <div>
                            <dt>Audio output</dt>
                            <dd>{voice.audioOutput}</dd>
                        </div>
                        <div>
                            <dt>Volume</dt>
                            <dd>{voice.volume}</dd>
                        </div>
                        <div>
                            <dt>Transpose</dt>
                            <dd>{voice.transpose}</dd>
                        </div>
                        <div>
                            <dt>Detune</dt>
                            <dd>{voice.detune}</dd>
                        </div>
                    </dl>
                {/each}
            {:else if detail.details.type === 'setup'}
                <h3>Wave slots</h3>
                <table>
                    <thead><tr><th>Slot</th><th>Name</th></tr></thead><tbody>
                        {#each detail.details.waves as wave}<tr><td>{wave.slot}</td><td>{wave.name}</td></tr>{/each}
                    </tbody>
                </table>
            {/if}
            <h3>Relationships</h3>
            {#each relationships as edge (edge.id)}
                <div class="relationship">
                    <strong>{edge.type}</strong><span class:unresolved={edge.quality !== 'RESOLVED'}
                        >{edge.quality}</span
                    >
                    <p>{edge.basis}</p>
                    {#if edge.sourceObjectId !== detail.objectId}<button
                            type="button"
                            onclick={() => onselect(edge.sourceObjectId)}>{objectName(edge.sourceObjectId)}</button
                        >{/if}
                    {#if edge.targetObjectId && edge.targetObjectId !== detail.objectId}<button
                            type="button"
                            onclick={() => onselect(edge.targetObjectId!)}>{objectName(edge.targetObjectId)}</button
                        >{/if}
                    {#each edge.candidateObjectIds as id}<button type="button" onclick={() => onselect(id)}
                            >Candidate: {objectName(id)}</button
                        >{/each}
                </div>
            {:else}<p>No relationships</p>{/each}
            <h3>Source locations</h3>
            {#each detail.locations as location}<dl>
                    <div>
                        <dt>Source</dt>
                        <dd>{sourceName(location.memberId)}</dd>
                    </div>
                    <div>
                        <dt>Entry</dt>
                        <dd>{location.entryId}</dd>
                    </div>
                    <div>
                        <dt>Byte range</dt>
                        <dd>{location.offset} + {location.length}</dd>
                    </div>
                </dl>{/each}
        {:else}<p>No object selected</p>{/if}
    </div>
    {#if detail}<InspectorModeFooter mode="files" onclick={() => detail && onrevealFiles(detail.objectId)} />{/if}
</aside>

<style>
    .tx-inspector {
        display: flex;
        flex-direction: column;
        min-height: 0;
        overflow: hidden;
        font-size: 11px;
    }
    header {
        padding: 10px;
        border-bottom: 1px solid var(--color-border);
    }
    h2 {
        font-size: 13px;
        margin: 0 0 4px;
        overflow-wrap: anywhere;
    }
    h3 {
        font-size: 11px;
        margin: 16px 0 8px;
    }
    h4 {
        font-size: 10px;
        margin: 8px 0;
    }
    .detail-scroll {
        overflow: auto;
        padding: 0 10px;
        flex: 1;
    }
    dl {
        margin: 8px 0;
    }
    dl div {
        display: grid;
        grid-template-columns: 100px 1fr;
        gap: 8px;
        padding: 3px 0;
    }
    dt {
        color: var(--color-text-muted);
    }
    dd {
        margin: 0;
        overflow-wrap: anywhere;
    }
    table {
        width: 100%;
        border-collapse: collapse;
    }
    th,
    td {
        text-align: left;
        padding: 4px;
        border-bottom: 1px solid var(--color-border);
    }
    th,
    header span {
        font-size: 10px;
        color: var(--color-text-muted);
    }
    .relationship {
        padding: 6px 0;
        border-bottom: 1px solid var(--color-border);
        overflow-wrap: anywhere;
    }
    .relationship span {
        margin-left: 8px;
    }
    .unresolved {
        color: #e0b765;
    }
    .relationship p {
        margin: 4px 0;
    }
    .relationship button {
        background: none;
        border: 0;
        color: var(--color-accent, #72b7cf);
        text-align: left;
        font-size: 11px;
        padding: 3px 0;
        display: block;
        overflow-wrap: anywhere;
    }
</style>
