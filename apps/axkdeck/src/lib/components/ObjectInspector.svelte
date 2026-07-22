<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import type { InspectorSelection } from '../types';
    import Icon from './Icon.svelte';
    import Waveform from './Waveform.svelte';

    interface Props {
        selection: InspectorSelection;
        onplay?: (objectId: string) => void;
        onstop?: () => void;
        playingObjectId?: string | null;
        preparingObjectId?: string | null;
        playheadFrame?: number;
    }

    let {
        selection,
        onplay = () => undefined,
        onstop = () => undefined,
        playingObjectId = null,
        preparingObjectId = null,
        playheadFrame = 0,
    }: Props = $props();
    const heading = $derived(
        selection?.kind === 'program'
            ? 'Program details'
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
                    <dt>Stored size</dt>
                    <dd>{formatStoredSize(selection.program.object.storedSizeBytes)}</dd>
                </div>
            </dl>
        </div>
    {:else if selection?.kind === 'sample-bank'}
        <div class="inspector-content">
            <div class="inspector-title">
                <span>Sample Bank (SBAC)</span>
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
                    <dt>Stored size</dt>
                    <dd>{formatStoredSize(selection.item.object.storedSizeBytes)}</dd>
                </div>
            </dl>
        </div>
    {:else if selection?.kind === 'sample'}
        {@const timelineFrameCount = Math.max(
            1,
            ...selection.waveData.map((member) => member.waveData.object.frameCount),
        )}
        <div class="inspector-content">
            {#if selection.waveData.length > 0}
                <div class="inspector-wave-stack">
                    {#each selection.waveData as member}
                        {@const laneLabel =
                            selection.waveData.length === 1 ? 'Wave Data' : member.role === 'left' ? 'Left' : 'Right'}
                        {@const laneAriaLabel =
                            selection.waveData.length === 1
                                ? `Wave Data ${member.waveData.name}`
                                : `${laneLabel} Wave Data ${member.waveData.name}`}
                        <div class="inspector-wave-lane" role="group" aria-label={laneAriaLabel}>
                            <div class="inspector-wave-label">
                                <span>{laneLabel}</span>
                                <strong title={member.waveData.name}>{member.waveData.name}</strong>
                            </div>
                            <div class="inspector-wave-canvas">
                                <Waveform
                                    values={member.waveData.waveform}
                                    sourceFrameCount={member.waveData.object.frameCount}
                                    {timelineFrameCount}
                                    playheadRatio={playingObjectId === selection.item.objectId
                                        ? playheadFrame / timelineFrameCount
                                        : 0}
                                />
                                {#if member.waveData.previewState === 'loading'}
                                    <span class="inspector-wave-state">Loading waveform</span>
                                {:else if member.waveData.previewState === 'failed'}
                                    <span class="inspector-wave-state error">Waveform unavailable</span>
                                {/if}
                            </div>
                        </div>
                    {/each}
                </div>
            {:else}
                <div class="inspector-wave-missing">No resolved Wave Data</div>
            {/if}
            <div class="inspector-title-row">
                <div class="inspector-title">
                    <span>Sample (SBNK)</span>
                    <h3>{selection.item.name}</h3>
                </div>
                <button
                    class="icon-button"
                    type="button"
                    aria-label={playingObjectId === selection.item.objectId
                        ? `Stop ${selection.item.name}`
                        : `Play ${selection.item.name}`}
                    title={preparingObjectId === selection.item.objectId
                        ? 'Preparing audio'
                        : playingObjectId === selection.item.objectId
                          ? 'Stop'
                          : 'Play'}
                    disabled={preparingObjectId === selection.item.objectId}
                    onclick={() => {
                        if (playingObjectId === selection.item.objectId) onstop();
                        else onplay(selection.item.objectId);
                    }}
                >
                    <Icon name={playingObjectId === selection.item.objectId ? 'stop' : 'play'} size={13} />
                </button>
            </div>
            <dl class="metadata-list">
                <div>
                    <dt>Sample Banks</dt>
                    <dd>{selection.memberships.length || 'Standalone'}</dd>
                </div>
                <div>
                    <dt>Wave Data</dt>
                    <dd>{selection.waveData.length}</dd>
                </div>
                <div>
                    <dt>Partition</dt>
                    <dd>{selection.item.object.partitionName || 'Unknown'}</dd>
                </div>
                <div>
                    <dt>Stored size</dt>
                    <dd>{formatStoredSize(selection.item.object.storedSizeBytes)}</dd>
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
            <div class="inspector-title-row">
                <div class="inspector-title">
                    <span>Wave Data (SMPL)</span>
                    <h3>{item.name}</h3>
                </div>
                <button
                    class="icon-button"
                    type="button"
                    aria-label={playingObjectId === item.objectKey ? `Stop ${item.name}` : `Play ${item.name}`}
                    title={preparingObjectId === item.objectKey
                        ? 'Preparing audio'
                        : playingObjectId === item.objectKey
                          ? 'Stop'
                          : 'Play'}
                    disabled={preparingObjectId === item.objectKey}
                    onclick={() => {
                        if (playingObjectId === item.objectKey) onstop();
                        else onplay(item.objectKey);
                    }}
                >
                    <Icon name={playingObjectId === item.objectKey ? 'stop' : 'play'} size={13} />
                </button>
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
