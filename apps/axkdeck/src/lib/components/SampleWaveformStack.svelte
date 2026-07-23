<script lang="ts">
    import type { LinkedWaveDataItem } from '../types';
    import Waveform from './Waveform.svelte';

    interface Props {
        waveData: LinkedWaveDataItem[];
        sampleObjectId: string;
        playingObjectId?: string | null;
        playheadFrame?: number;
    }

    let { waveData, sampleObjectId, playingObjectId = null, playheadFrame = 0 }: Props = $props();
    const timelineFrameCount = $derived(Math.max(1, ...waveData.map((member) => member.waveData.object.frameCount)));
</script>

{#if waveData.length > 0}
    <div class="inspector-wave-stack">
        {#each waveData as member}
            {@const laneLabel = waveData.length === 1 ? 'Wave Data' : member.role === 'left' ? 'Left' : 'Right'}
            {@const laneAriaLabel =
                waveData.length === 1
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
                        playheadRatio={playingObjectId === sampleObjectId ? playheadFrame / timelineFrameCount : 0}
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
