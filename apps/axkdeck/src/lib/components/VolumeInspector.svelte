<script lang="ts">
    import type { DiskTreeItem } from '../types';
    import { useVolumeCapacity } from '../volumeCapacity.svelte';
    import { formatStoredSize } from '../formatBytes';
    import InspectorSection from './InspectorSection.svelte';
    import Icon from './Icon.svelte';
    import VolumeCapacityProfile from './VolumeCapacityProfile.svelte';

    let { item }: { item: DiskTreeItem } = $props();
    const capacity = useVolumeCapacity();
    const inspectionState = $derived(capacity?.state(item.id));
    const violation = $derived(
        inspectionState?.status === 'ready' &&
            inspectionState.inspection.report.profiles.some((profile) => profile.status === 'DOES_NOT_FIT'),
    );
    $effect(() => {
        void capacity?.inspect(item.id);
    });
</script>

<aside class="inspector" aria-label="Volume inspector">
    <div class="panel-heading">
        <div>
            <p class="eyebrow">Inspector</p>
            <h2>Volume details</h2>
        </div>
    </div>
    <div class="inspector-body">
        <div class="inspector-content">
            <div class="inspector-title">
                <span>Volume</span>
                <h3 style:white-space="pre-wrap">{item.name}</h3>
            </div>
            <InspectorSection scope="volume" sectionId="properties" title="Properties">
                <dl class="metadata-list">
                    <div>
                        <dt>Partition</dt>
                        <dd>{item.partitionIndex === undefined ? 'Unknown' : item.partitionIndex + 1}</dd>
                    </div>
                    {#if item.sizeBytes !== undefined}<div>
                            <dt>Stored size</dt>
                            <dd>{formatStoredSize(item.sizeBytes)}</dd>
                        </div>{/if}
                </dl>
            </InspectorSection>
            <InspectorSection
                scope="volume"
                sectionId="sampler-capacity"
                title="Sampler Capacity"
                defaultExpanded={violation}
                stateKey={capacity?.inspectionKey(item.id) ?? item.id}
            >
                {#if inspectionState?.status === 'error'}
                    <p class="capacity-note" role="alert">{inspectionState.message}</p>
                    <button
                        class="icon-button"
                        type="button"
                        aria-label="Retry capacity inspection"
                        title="Retry capacity inspection"
                        onclick={() => void capacity?.inspect(item.id, true)}><Icon name="refresh" size={14} /></button
                    >
                {:else if inspectionState?.status === 'ready'}
                    {#each inspectionState.inspection.report.profiles as profile (profile.target)}
                        <VolumeCapacityProfile {profile} help />
                    {/each}
                {:else}<p class="capacity-note" role="status">Inspecting capacity...</p>{/if}
            </InspectorSection>
        </div>
    </div>
</aside>

<style>
    .capacity-note {
        margin: 3px 0 0;
        font-size: 8.5px;
        line-height: 12px;
        color: var(--color-text-muted);
        overflow-wrap: anywhere;
    }
</style>
