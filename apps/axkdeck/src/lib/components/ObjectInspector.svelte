<script lang="ts">
    import { formatSequenceTempo, laterTempoChangeCount } from '../sequenceTempo';
    import { formatStoredSize } from '../formatBytes';
    import type { InspectorSelection } from '../types';
    import SampleWaveformStack from './SampleWaveformStack.svelte';
    import Waveform from './Waveform.svelte';

    interface Props {
        selection: InspectorSelection;
        playingObjectId?: string | null;
        playheadFrame?: number;
    }

    let { selection, playingObjectId = null, playheadFrame = 0 }: Props = $props();
    function formatDependencySize(size: number | null): string {
        return size === null ? 'Unavailable' : formatStoredSize(size);
    }
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
</script>

<aside class="inspector" aria-label="Object inspector">
    <div class="panel-heading">
        <div>
            <p class="eyebrow">Inspector</p>
            <h2>{heading}</h2>
        </div>
    </div>

    {#if selection?.kind === 'program'}
        <div class="inspector-content">
            <div class="inspector-title">
                <span>PROG {selection.program.slot}</span>
                <h3>{selection.program.name}</h3>
            </div>
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
        </div>
    {:else if selection?.kind === 'sequence'}
        {@const item = selection.sequence}
        {@const metadata = item.object.sequence}
        <div class="inspector-content">
            <div class="inspector-title">
                <span>Sequence</span>
                <h3>{item.name}</h3>
            </div>
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
        </div>
    {:else if selection?.kind === 'sample-bank'}
        {@const displayedMember =
            selection.memberPreviews.find((member) => member.item.objectId === selection.displayedMemberId) ??
            selection.memberPreviews[0]}
        {@const displayedMemberIndex = displayedMember ? selection.memberPreviews.indexOf(displayedMember) : -1}
        <div class="inspector-content">
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
            <div class="inspector-title">
                <span>Sample Bank</span>
                <h3>{selection.item.name}</h3>
            </div>
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
        </div>
    {:else if selection?.kind === 'sample'}
        <div class="inspector-content">
            <SampleWaveformStack
                preview={selection.preview}
                sampleObjectId={selection.item.objectId}
                {playingObjectId}
                {playheadFrame}
            />
            <div class="inspector-title">
                <span>Sample</span>
                <h3>{selection.item.name}</h3>
            </div>
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
        </div>
    {:else if selection?.kind === 'wave-data'}
        {@const item = selection.waveData}
        <div class="inspector-content">
            <div class="inspector-wave">
                <Waveform
                    values={item.waveform}
                    large
                    sourceFrameCount={item.object.frameCount}
                    timelineFrameCount={item.object.frameCount}
                    playheadRatio={playingObjectId === item.objectKey && item.object.frameCount > 0
                        ? playheadFrame / item.object.frameCount
                        : 0}
                />
            </div>
            <div class="inspector-title">
                <span>Wave Data</span>
                <h3>{item.name}</h3>
            </div>
            <dl class="metadata-list">
                <div>
                    <dt>Root key</dt>
                    <dd>{item.note}</dd>
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
                    <dd>{item.sampleRate}</dd>
                </div>
                <div>
                    <dt>Format</dt>
                    <dd>{item.bitDepth} {item.channels}</dd>
                </div>
                <div>
                    <dt>Loop mode</dt>
                    <dd>{item.object.loopModeLabel || 'Unknown'}</dd>
                </div>
                <div>
                    <dt>Loop start</dt>
                    <dd>{item.object.loopStartFrame ?? 0} frames</dd>
                </div>
                <div>
                    <dt>Loop length</dt>
                    <dd>{item.object.loopLengthFrames ?? 0} frames</dd>
                </div>
                <div>
                    <dt>Stored size</dt>
                    <dd>{formatStoredSize(item.storedSizeBytes)}</dd>
                </div>
            </dl>
        </div>
    {:else}
        <div class="inspector-empty">
            <p class="empty-copy">No object selected</p>
        </div>
    {/if}
</aside>
