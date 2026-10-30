<script lang="ts">
    import type { components } from '../../../lib/generated/axklibApiV1';
    import type { PreviewEnvelope, OpenedImage } from '../../../lib/transport';
    import InspectorShell from '../../../lib/components/InspectorShell.svelte';
    import InspectorSection from '../../../lib/components/InspectorSection.svelte';
    import Waveform from '../../../lib/components/Waveform.svelte';
    import { previewTimeline } from '../../../lib/audio/documentAudio.svelte';
    import { waveformPlayheadRatio } from '../../../lib/waveformTimeline';
    import InspectorModeFooter from '../../../lib/components/InspectorModeFooter.svelte';
    let {
        detail,
        preview,
        audioError = '',
        playingObjectId = null,
        playheadFrame = 0,
        relationships,
        objects,
        sources,
        onselect,
        onrevealFiles,
    }: {
        detail: components['schemas']['Tx16wObjectDetail'] | null;
        preview?: PreviewEnvelope;
        audioError?: string;
        playingObjectId?: string | null;
        playheadFrame?: number;
        relationships: components['schemas']['ImageRelationshipItem'][];
        objects: components['schemas']['ImageObjectItem'][];
        sources: OpenedImage['sources'];
        onselect: (id: string) => void;
        onrevealFiles: (id: string) => void;
    } = $props();
    const labels: Record<string, string> = {
        sampleRate: 'Sample rate (Hz)',
        nativeBitDepth: 'Native bit depth',
        attackFrames: 'Attack frames',
        repeatFrames: 'Repeat frames',
        looped: 'Looped',
        slot: 'Slot',
        waveSlot: 'Wave slot',
        rootKey: 'Root key',
        writeProtected: 'Write protected',
        kind: 'Native kind',
    };
    const scope = $derived(`yamaha.tx16w:${detail?.type ?? 'empty'}`);
    const timeline = $derived(previewTimeline(preview));
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

{#snippet footer()}
    {#if detail}<InspectorModeFooter mode="files" onclick={() => detail && onrevealFiles(detail.objectId)} />{/if}
{/snippet}
<InspectorShell heading="Object details" label="TX16W inspector" {footer}>
    {#if detail}
        <div class="inspector-title">
            <span>{detail.type}</span>
            <h3>{detail.name}</h3>
        </div>
        {#if detail.details.type === 'wave'}
            <InspectorSection
                {scope}
                sectionId="preview"
                title="Preview"
                class="inspector-preview"
                defaultExpanded={true}
            >
                <div class="inspector-wave">
                    <Waveform
                        values={preview?.lanes[0]?.bins ?? []}
                        large
                        {timeline}
                        playheadRatio={playingObjectId === detail.objectId
                            ? waveformPlayheadRatio(timeline, playheadFrame, timeline.sampleRate)
                            : 0}
                    />
                </div>
                {#if audioError}<p role="alert">{audioError}</p>{/if}
                {#each preview?.warnings ?? [] as warning}<p class="warning">{warning}</p>{/each}
                {#if detail.details.sampleRateApproximate && !preview}
                    <p class="warning">Sample rate is uncertain; audition uses 33,333 Hz. Duration is approximate.</p>
                {/if}
            </InspectorSection>
        {/if}
        <InspectorSection {scope} sectionId="properties" title="Properties" defaultExpanded={true}>
            <dl class="metadata-list">
                {#each Object.entries(detail.details).filter(([key, value]) => key !== 'type' && key !== 'sampleRateApproximate' && !Array.isArray(value)) as [key, value] (key)}
                    <div>
                        <dt>{labels[key] ?? key}</dt>
                        <dd>{typeof value === 'boolean' ? (value ? 'Yes' : 'No') : value}</dd>
                    </div>
                {/each}
            </dl>
        </InspectorSection>
        {#if detail.details.type === 'voice'}
            <InspectorSection {scope} sectionId="key-regions" title="Key regions" defaultExpanded={true}>
                <table>
                    <thead><tr><th>Timbre</th><th>Keys</th><th>Fade</th></tr></thead><tbody>
                        {#each detail.details.regions as region}<tr
                                ><td>{region.timbreSlot}</td><td>{region.lowKey}-{region.highKey}</td><td
                                    >{region.fade}</td
                                ></tr
                            >{/each}
                    </tbody>
                </table>
            </InspectorSection>
        {:else if detail.details.type === 'performance'}
            <InspectorSection {scope} sectionId="voice-assignments" title="Voice assignments" defaultExpanded={true}>
                {#each detail.details.voices as voice, index}
                    <h4>Part {index + 1}</h4>
                    <dl class="metadata-list">
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
            </InspectorSection>
        {:else if detail.details.type === 'setup'}
            <InspectorSection {scope} sectionId="wave-slots" title="Wave slots" defaultExpanded={true}>
                <table>
                    <thead><tr><th>Slot</th><th>Name</th></tr></thead><tbody>
                        {#each detail.details.waves as wave}<tr><td>{wave.slot}</td><td>{wave.name}</td></tr>{/each}
                    </tbody>
                </table>
            </InspectorSection>
        {/if}
        <InspectorSection {scope} sectionId="relationships" title="Relationships" defaultExpanded={false}>
            {#each relationships as edge (edge.id)}
                <div class="inspector-relationship-group">
                    <h5>{edge.type}</h5>
                    <dl class="metadata-list">
                        <div>
                            <dt>Resolution</dt>
                            <dd class:unresolved={edge.quality !== 'RESOLVED'}>{edge.quality}</dd>
                        </div>
                        <div>
                            <dt>Reference</dt>
                            <dd>{edge.basis}</dd>
                        </div>
                    </dl>
                    <ul>
                        {#if edge.sourceObjectId !== detail.objectId}<li>
                                <button type="button" onclick={() => onselect(edge.sourceObjectId)}
                                    ><strong>{objectName(edge.sourceObjectId)}</strong></button
                                >
                            </li>{/if}
                        {#if edge.targetObjectId && edge.targetObjectId !== detail.objectId}<li>
                                <button type="button" onclick={() => onselect(edge.targetObjectId!)}
                                    ><strong>{objectName(edge.targetObjectId)}</strong></button
                                >
                            </li>{/if}
                        {#each edge.candidateObjectIds as id}<li>
                                <button type="button" onclick={() => onselect(id)}
                                    ><strong>Candidate: {objectName(id)}</strong></button
                                >
                            </li>{/each}
                    </ul>
                </div>
            {:else}<p class="inspector-relationships-empty">No relationships</p>{/each}
        </InspectorSection>
        <InspectorSection {scope} sectionId="stored-format" title="Stored format" defaultExpanded={false}>
            <dl class="metadata-list">
                <div>
                    <dt>Format</dt>
                    <dd>{detail.formatId}</dd>
                </div>
                <div>
                    <dt>Profile</dt>
                    <dd>{detail.profile}</dd>
                </div>
            </dl>
            {#each detail.locations as location}<dl class="metadata-list">
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
        </InspectorSection>
    {:else}<div class="inspector-empty"><p class="empty-copy">No object selected</p></div>{/if}
</InspectorShell>

<style>
    .warning {
        color: var(--color-warning);
        font-size: 11px;
    }
    table {
        width: 100%;
        border-collapse: collapse;
        font-size: 8.5px;
    }
    th,
    td {
        text-align: left;
        padding: 4px;
        border-bottom: 1px solid var(--color-border);
    }
    th {
        font-size: 10px;
        color: var(--color-text-muted);
    }
    h4 {
        margin: 8px 0 2px;
        font-size: 10px;
        color: var(--color-text-muted);
    }
    .unresolved {
        color: var(--color-warning);
    }
</style>
