<script lang="ts">
    import { onDestroy } from 'svelte';
    import { formatSequenceTempo, laterTempoChangeCount } from '../sequenceTempo';
    import { formatStoredSize } from '../formatBytes';
    import type { InspectorSelection } from '../types';
    import { waveformPlayheadRatio, type WaveformTimeline } from '../waveformTimeline';
    import Icon from './Icon.svelte';
    import InspectorRelationships from './InspectorRelationships.svelte';
    import SampleWaveformStack from './SampleWaveformStack.svelte';
    import Waveform from './Waveform.svelte';

    interface Props {
        selection: InspectorSelection;
        playingObjectId?: string | null;
        playheadFrame?: number;
        onrelationshipnavigate?: (objectId: string, focusTarget: boolean) => void;
        onmetadatacopy?: (objectId: string) => Promise<void>;
    }

    let {
        selection,
        playingObjectId = null,
        playheadFrame = 0,
        onrelationshipnavigate,
        onmetadatacopy,
    }: Props = $props();
    let copyState = $state<'idle' | 'copying' | 'copied'>('idle');
    let copyRequest = 0;
    let copiedTimer: ReturnType<typeof setTimeout> | undefined;
    function formatDependencySize(size: number | null): string {
        return size === null ? 'Unavailable' : formatStoredSize(size);
    }
    function formatObjectEncoding(encoding: string): string {
        if (encoding === 'current') return 'Current A-series';
        if (encoding === 'alternating-byte') return 'Recovered conversion artifact';
        return 'Unknown';
    }
    function formatLoopMode(label: string | undefined, raw: number | undefined): string {
        if (label) return label;
        return raw === undefined ? 'Unknown' : `Unknown (${raw})`;
    }
    function formatFrames(frames: number | undefined): string {
        return `${(frames ?? 0).toLocaleString()} frames`;
    }
    function formatStorageState(state: 'COMPLETE' | 'INCOMPLETE'): string {
        return state === 'COMPLETE' ? 'Complete' : 'Incomplete';
    }
    const selectedObjectId = $derived(
        selection?.kind === 'program'
            ? selection.program.objectId
            : selection?.kind === 'sequence'
              ? selection.sequence.objectId
              : selection?.kind === 'sample-bank' || selection?.kind === 'sample'
                ? selection.item.objectId
                : selection?.kind === 'wave-data'
                  ? selection.waveData.objectKey
                  : '',
    );
    const heading = $derived(
        selection?.kind === 'program'
            ? 'Program details'
            : selection?.kind === 'sequence'
              ? 'Sequence details'
              : selection?.kind === 'sample-bank'
                ? 'Sample Bank details'
                : selection?.kind === 'sample'
                  ? 'Sample details'
                  : selection?.kind === 'wave-data'
                    ? 'Wave Data details'
                    : 'Object details',
    );

    $effect(() => {
        selectedObjectId;
        copyRequest += 1;
        copyState = 'idle';
        clearTimeout(copiedTimer);
    });

    async function copyMetadata(): Promise<void> {
        if (!selectedObjectId || !onmetadatacopy || copyState === 'copying') return;
        const request = ++copyRequest;
        copyState = 'copying';
        try {
            await onmetadatacopy(selectedObjectId);
            if (request !== copyRequest) return;
            copyState = 'copied';
            copiedTimer = setTimeout(() => {
                if (request === copyRequest) copyState = 'idle';
            }, 1_500);
        } catch {
            if (request === copyRequest) copyState = 'idle';
        }
    }

    onDestroy(() => clearTimeout(copiedTimer));
</script>

<aside class="inspector" aria-label="Object inspector">
    <div class="panel-heading">
        <div>
            <p class="eyebrow">Inspector</p>
            <h2>{heading}</h2>
        </div>
        {#if selectedObjectId && onmetadatacopy}
            <button
                class="icon-button inspector-copy-button"
                type="button"
                aria-label={copyState === 'copied' ? 'Object metadata copied' : 'Copy object metadata'}
                title={copyState === 'copied' ? 'Object metadata copied' : 'Copy object metadata as JSON'}
                disabled={copyState === 'copying'}
                onclick={() => void copyMetadata()}
            >
                <Icon name={copyState === 'copied' ? 'check' : 'copy'} size={14} />
            </button>
            <span class="inspector-copy-status" aria-live="polite">
                {copyState === 'copied' ? 'Object metadata copied' : ''}
            </span>
        {/if}
    </div>

    <div class="inspector-body">
        {#if selection?.kind === 'program'}
            <div class="inspector-content">
                <div class="inspector-title">
                    <span>PROG {selection.program.slot}</span>
                    <h3>{selection.program.name}</h3>
                </div>
                <section class="inspector-section" aria-labelledby="inspector-properties-heading">
                    <h4 id="inspector-properties-heading">Properties</h4>
                    <dl class="metadata-list">
                        <div>
                            <dt>Assignments</dt>
                            <dd>{selection.assignments.length}</dd>
                        </div>
                        <div>
                            <dt>Partition</dt>
                            <dd>{selection.program.object.partitionName || 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Volume</dt>
                            <dd>{selection.program.object.volumeName || 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Object size</dt>
                            <dd>{formatStoredSize(selection.program.object.storedSizeBytes)}</dd>
                        </div>
                        <div>
                            <dt>Object size with deps.</dt>
                            <dd>{formatDependencySize(selection.program.object.sizeWithDependenciesBytes)}</dd>
                        </div>
                    </dl>
                </section>
            </div>
        {:else if selection?.kind === 'sequence'}
            {@const item = selection.sequence}
            {@const metadata = item.object.sequence}
            <div class="inspector-content">
                <div class="inspector-title">
                    <span>Sequence</span>
                    <h3>{item.name}</h3>
                </div>
                <section class="inspector-section" aria-labelledby="inspector-properties-heading">
                    <h4 id="inspector-properties-heading">Properties</h4>
                    <dl class="metadata-list">
                        <div>
                            <dt>Events</dt>
                            <dd>{metadata?.eventCount.toLocaleString() ?? 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Timing</dt>
                            <dd>{metadata?.ticksPerQuarterNote ?? 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Initial tempo</dt>
                            <dd>
                                {metadata === undefined
                                    ? 'Unknown'
                                    : formatSequenceTempo(metadata.effectiveInitialTempoMicrosecondsPerQuarterNote)}
                            </dd>
                        </div>
                        {#if metadata?.headerTempoBpm !== undefined}
                            <div>
                                <dt>Sampler header tempo</dt>
                                <dd>{metadata.headerTempoBpm.toLocaleString()} BPM</dd>
                            </div>
                        {/if}
                        <div>
                            <dt>Tempo changes</dt>
                            <dd>
                                {#if metadata}
                                    {@const count = laterTempoChangeCount(metadata.tempoEvents)}
                                    {count.toLocaleString()}
                                    {count === 1 ? 'change' : 'changes'}
                                {:else}
                                    Unknown
                                {/if}
                            </dd>
                        </div>
                        <div>
                            <dt>First tick</dt>
                            <dd>{metadata?.firstTick.toLocaleString() ?? 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>End tick</dt>
                            <dd>{metadata?.endTick.toLocaleString() ?? 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Partition</dt>
                            <dd>{item.object.partitionName || 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Volume</dt>
                            <dd>{item.object.volumeName || 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Stored size</dt>
                            <dd>{formatStoredSize(item.object.storedSizeBytes)}</dd>
                        </div>
                    </dl>
                </section>
            </div>
        {:else if selection?.kind === 'sample-bank'}
            {@const displayedMember =
                selection.memberPreviews.find((member) => member.item.objectId === selection.displayedMemberId) ??
                selection.memberPreviews[0]}
            {@const displayedMemberIndex = displayedMember ? selection.memberPreviews.indexOf(displayedMember) : -1}
            <div class="inspector-content">
                <div class="inspector-title">
                    <span>Sample Bank</span>
                    <h3>{selection.item.name}</h3>
                </div>
                <section class="inspector-preview inspector-section" aria-labelledby="inspector-preview-heading">
                    <h4 id="inspector-preview-heading">Preview</h4>
                    {#if displayedMember}
                        <div class="inspector-bank-sample-heading inspector-inline-heading">
                            <span>Sample {displayedMemberIndex + 1} of {selection.memberPreviews.length}</span>
                            <strong title={displayedMember.item.name}>{displayedMember.item.name}</strong>
                        </div>
                        <SampleWaveformStack
                            preview={displayedMember}
                            sampleObjectId={displayedMember.item.objectId}
                            {playingObjectId}
                            {playheadFrame}
                        />
                    {:else}
                        <div class="inspector-wave-missing">No Samples</div>
                    {/if}
                </section>
                <section class="inspector-section" aria-labelledby="inspector-properties-heading">
                    <h4 id="inspector-properties-heading">Properties</h4>
                    <dl class="metadata-list">
                        <div>
                            <dt>Samples</dt>
                            <dd>{selection.members.length}</dd>
                        </div>
                        <div>
                            <dt>Partition</dt>
                            <dd>{selection.item.object.partitionName || 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Volume</dt>
                            <dd>{selection.item.object.volumeName || 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Object size</dt>
                            <dd>{formatStoredSize(selection.item.object.storedSizeBytes)}</dd>
                        </div>
                        <div>
                            <dt>Object size with deps.</dt>
                            <dd>{formatDependencySize(selection.item.object.sizeWithDependenciesBytes)}</dd>
                        </div>
                    </dl>
                </section>
            </div>
        {:else if selection?.kind === 'sample'}
            <div class="inspector-content">
                <div class="inspector-title">
                    <span>Sample</span>
                    <h3>{selection.item.name}</h3>
                </div>
                <section class="inspector-preview inspector-section" aria-labelledby="inspector-preview-heading">
                    <h4 id="inspector-preview-heading">Preview</h4>
                    <SampleWaveformStack
                        preview={selection.preview}
                        sampleObjectId={selection.item.objectId}
                        {playingObjectId}
                        {playheadFrame}
                    />
                </section>
                <section class="inspector-section" aria-labelledby="inspector-properties-heading">
                    <h4 id="inspector-properties-heading">Properties</h4>
                    <dl class="metadata-list">
                        <div>
                            <dt>Sample Banks</dt>
                            <dd>{selection.memberships.length || 'Standalone'}</dd>
                        </div>
                        <div>
                            <dt>Wave Data</dt>
                            <dd>{selection.preview.waveData.length}</dd>
                        </div>
                        <div>
                            <dt>Partition</dt>
                            <dd>{selection.item.object.partitionName || 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Object size</dt>
                            <dd>{formatStoredSize(selection.item.object.storedSizeBytes)}</dd>
                        </div>
                        <div>
                            <dt>Object size with deps.</dt>
                            <dd>{formatDependencySize(selection.item.object.sizeWithDependenciesBytes)}</dd>
                        </div>
                    </dl>
                </section>
            </div>
        {:else if selection?.kind === 'wave-data'}
            {@const item = selection.waveData}
            {@const embeddedContainerName = item.object.embeddedContainerName?.trim() ?? ''}
            {@const waveEndFrame = item.object.waveStartFrame + item.object.waveLengthFrames}
            {@const loopEndFrame = (item.object.loopStartFrame ?? 0) + (item.object.loopLengthFrames ?? 0)}
            {@const timeline: WaveformTimeline = {
                sampleRate: item.object.sampleRate,
                storedFrameCount: item.object.storedFrameCount,
                playbackStartFrame: item.object.waveStartFrame,
                playbackLengthFrames: item.object.waveLengthFrames,
                loopStartFrame: item.object.loopStartFrame ?? 0,
                loopLengthFrames: item.object.loopLengthFrames ?? 0,
                displayDurationSeconds:
                    item.object.sampleRate > 0 ? item.object.storedFrameCount / item.object.sampleRate : 0,
            }}
            <div class="inspector-content">
                <div class="inspector-title">
                    <span>Wave Data</span>
                    <h3>{item.name}</h3>
                </div>
                <section class="inspector-preview inspector-section" aria-labelledby="inspector-preview-heading">
                    <h4 id="inspector-preview-heading">Preview</h4>
                    <div class="inspector-wave">
                        <Waveform
                            values={item.waveform}
                            large
                            {timeline}
                            playheadRatio={playingObjectId === item.objectKey && item.object.storedFrameCount > 0
                                ? waveformPlayheadRatio(timeline, playheadFrame, item.object.sampleRate)
                                : 0}
                        />
                    </div>
                </section>
                <section class="inspector-section" aria-labelledby="inspector-properties-heading">
                    <h4 id="inspector-properties-heading">Properties</h4>
                    <dl class="metadata-list">
                        <div>
                            <dt>Object encoding</dt>
                            <dd>{formatObjectEncoding(item.object.objectEncoding)}</dd>
                        </div>
                        <div>
                            <dt>Directory entry</dt>
                            <dd>{item.object.directoryEntryName || 'Unknown'}</dd>
                        </div>
                        {#if embeddedContainerName && embeddedContainerName !== item.object.volumeName.trim()}
                            <div>
                                <dt>Embedded container</dt>
                                <dd>{embeddedContainerName}</dd>
                            </div>
                        {/if}
                        <div>
                            <dt>Partition</dt>
                            <dd>{item.object.partitionName || 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Volume</dt>
                            <dd>{item.object.volumeName || 'Unknown'}</dd>
                        </div>
                        <div>
                            <dt>Root key</dt>
                            <dd>{item.note} ({item.object.rootKey})</dd>
                        </div>
                        <div>
                            <dt>Fine tune</dt>
                            <dd>{item.object.fineTuneCents ?? 0} cents</dd>
                        </div>
                        <div>
                            <dt>Duration</dt>
                            <dd>{item.duration}</dd>
                        </div>
                        <div>
                            <dt>Sample rate</dt>
                            <dd>{item.object.sampleRate.toLocaleString()} Hz</dd>
                        </div>
                        <div>
                            <dt>Audio format</dt>
                            <dd>{item.bitDepth} {item.channels}</dd>
                        </div>
                        <div>
                            <dt>Loop mode</dt>
                            <dd>{formatLoopMode(item.object.loopModeLabel, item.object.loopMode)}</dd>
                        </div>
                        <div>
                            <dt>Storage</dt>
                            <dd>{formatStorageState(item.object.storageState)}</dd>
                        </div>
                        <div>
                            <dt>Stored frames</dt>
                            <dd>{formatFrames(item.object.storedFrameCount)}</dd>
                        </div>
                        <div>
                            <dt>Wave start</dt>
                            <dd>{formatFrames(item.object.waveStartFrame)}</dd>
                        </div>
                        <div>
                            <dt>Wave end (exclusive)</dt>
                            <dd>{formatFrames(waveEndFrame)}</dd>
                        </div>
                        <div>
                            <dt>Wave length</dt>
                            <dd>{formatFrames(item.object.waveLengthFrames)}</dd>
                        </div>
                        <div>
                            <dt>Loop start</dt>
                            <dd>{formatFrames(item.object.loopStartFrame)}</dd>
                        </div>
                        <div>
                            <dt>Loop end (exclusive)</dt>
                            <dd>{formatFrames(loopEndFrame)}</dd>
                        </div>
                        <div>
                            <dt>Loop length</dt>
                            <dd>{formatFrames(item.object.loopLengthFrames)}</dd>
                        </div>
                        <div>
                            <dt>Stored size</dt>
                            <dd>{formatStoredSize(item.storedSizeBytes)}</dd>
                        </div>
                    </dl>
                </section>
            </div>
        {:else}
            <div class="inspector-empty">
                <p class="empty-copy">No object selected</p>
            </div>
        {/if}
        {#if selection}
            <InspectorRelationships groups={selection.relationships ?? []} onnavigate={onrelationshipnavigate} />
        {/if}
    </div>
</aside>
