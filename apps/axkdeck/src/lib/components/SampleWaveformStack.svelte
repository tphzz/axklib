<script lang="ts">
    import type { PreviewLane } from '../transport';
    import type { LinkedWaveDataItem, SampleWaveformPreview, WaveformBin } from '../types';
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
        frameCount: number;
        bins: readonly WaveformBin[];
        linked: LinkedWaveDataItem | undefined;
    }

    let { preview, sampleObjectId, playingObjectId = null, playheadFrame = 0 }: Props = $props();
    const timelineFrameCount = $derived(Math.max(1, preview.preview?.frameCount ?? 0));
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
            frameCount: 0,
            bins: [],
            linked: member,
        }));
    });
</script>

{#if preview.waveData.length > 0}
    <div class="inspector-wave-stack">
        {#each lanes as lane}
            {@const laneLabel = lanes.length === 1 ? 'Wave Data' : lane.role === 'LEFT' ? 'Left' : 'Right'}
            {@const waveDataName = lane.linked?.waveData.name ?? lane.sourceObjectId}
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
                        sourceFrameCount={lane.frameCount}
                        {timelineFrameCount}
                        playheadRatio={playingObjectId === sampleObjectId ? playheadFrame / timelineFrameCount : 0}
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
