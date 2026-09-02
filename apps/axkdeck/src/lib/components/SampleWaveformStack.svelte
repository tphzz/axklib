<script lang="ts">
    import type { PreviewLane } from '../transport';
    import type { LinkedWaveDataItem, SampleWaveformPreview, WaveformBin } from '../types';
    import { waveformPlayheadRatio, type WaveformTimeline } from '../waveformTimeline';
    import Waveform from './Waveform.svelte';

    interface Props {
        preview: SampleWaveformPreview;
        sampleObjectId: string;
        playingObjectId?: string | null;
        playheadFrame?: number;
    }

    interface DisplayLane {
        role: PreviewLane['role'];
        sourceObjectId: string;
        sampleRate: number;
        storedFrameCount: number;
        playbackStartFrame: number;
        playbackLengthFrames: number;
        loopStartFrame: number;
        loopLengthFrames: number;
        bins: readonly WaveformBin[];
        linked: LinkedWaveDataItem | undefined;
    }

    let { preview, sampleObjectId, playingObjectId = null, playheadFrame = 0 }: Props = $props();
    const lanes = $derived.by<DisplayLane[]>(() => {
        if (preview.preview) {
            return preview.preview.lanes.map((lane) => ({
                ...lane,
                linked:
                    preview.waveData.find((member) => member.waveData.objectKey === lane.sourceObjectId) ??
                    preview.waveData.find(
                        (member) =>
                            member.role === (lane.role === 'RIGHT' ? 'right' : lane.role === 'LEFT' ? 'left' : null),
                    ),
            }));
        }
        return preview.waveData.map((member) => ({
            role: member.role === 'left' ? 'LEFT' : 'RIGHT',
            sourceObjectId: member.waveData.objectKey,
            sampleRate: member.waveData.object.sampleRate,
            storedFrameCount: member.waveData.object.storedFrameCount,
            playbackStartFrame: member.waveData.object.waveStartFrame,
            playbackLengthFrames: member.waveData.object.waveLengthFrames,
            loopStartFrame: member.waveData.object.loopStartFrame ?? 0,
            loopLengthFrames: member.waveData.object.loopLengthFrames ?? 0,
            bins: [],
            linked: member,
        }));
    });
    const displayDurationSeconds = $derived(
        Math.max(0, ...lanes.map((lane) => (lane.sampleRate > 0 ? lane.storedFrameCount / lane.sampleRate : 0))),
    );
    const playbackSampleRate = $derived(lanes[0]?.sampleRate ?? 0);
</script>

{#if preview.waveData.length > 0}
    <div class="inspector-wave-stack">
        {#each lanes as lane}
            {@const laneLabel = lanes.length === 1 ? 'Wave Data' : lane.role === 'LEFT' ? 'Left' : 'Right'}
            {@const waveDataName = lane.linked?.waveData.name ?? lane.sourceObjectId}
            {@const timeline: WaveformTimeline = {
                sampleRate: lane.sampleRate,
                storedFrameCount: lane.storedFrameCount,
                playbackStartFrame: lane.playbackStartFrame,
                playbackLengthFrames: lane.playbackLengthFrames,
                loopStartFrame: lane.loopStartFrame,
                loopLengthFrames: lane.loopLengthFrames,
                displayDurationSeconds,
            }}
            {@const laneAriaLabel =
                lanes.length === 1 ? `Wave Data ${waveDataName}` : `${laneLabel} Wave Data ${waveDataName}`}
            <div class="inspector-wave-lane" role="group" aria-label={laneAriaLabel}>
                <div class="inspector-wave-label inspector-inline-heading">
                    <span>{laneLabel}</span>
                    <strong title={waveDataName}>{waveDataName}</strong>
                </div>
                <div class="inspector-wave-canvas">
                    <Waveform
                        values={lane.bins}
                        {timeline}
                        playheadRatio={playingObjectId === sampleObjectId
                            ? waveformPlayheadRatio(timeline, playheadFrame, playbackSampleRate)
                            : 0}
                    />
                    {#if preview.previewState === 'loading'}
                        <span class="inspector-wave-state">Loading waveform</span>
                    {:else if preview.previewState === 'failed'}
                        <span class="inspector-wave-state error">Waveform unavailable</span>
                    {/if}
                </div>
            </div>
        {/each}
    </div>
{:else}
    <div class="inspector-wave-missing">No resolved Wave Data</div>
{/if}
