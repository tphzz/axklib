<script lang="ts">
    import type { VolumeCapacityProfile } from '../volumeInspections';
    import {
        capacityMemoryHelp,
        capacityMetric,
        capacitySlotsHelp,
        capacityStatus,
    } from '../volumeCapacityPresentation';
    import AttributeHelp from './AttributeHelp.svelte';

    let { profile, help = false }: { profile: VolumeCapacityProfile; help?: boolean } = $props();
    const name = $derived(profile.target === 'A3000' ? 'A3000' : 'A4000/A5000');
    const reasons = $derived(
        profile.reasons.filter(
            (reason, index, all) =>
                reason.code !== 'RESIDENT_MINIMUM_EXCEEDS_POOL' &&
                all.findIndex((other) => other.message === reason.message) === index,
        ),
    );
</script>

<div class="capacity-profile" role="group" aria-label={`${name} capacity`}>
    <div class="capacity-profile-heading">
        <h5>{name}</h5>
        <span class:failure={profile.status === 'DOES_NOT_FIT'}>{capacityStatus(profile.status)}</span>
    </div>
    <dl class="metadata-list capacity-metrics">
        <div>
            <dt>
                {#if help}<AttributeHelp
                        label="Parameter memory"
                        description={capacityMemoryHelp(profile)}
                    />{:else}Parameter memory{/if}
            </dt>
            <dd>{capacityMetric(profile.peakBytes, profile.minimumResidentBytes, profile.parameterByteLimit, 'B')}</dd>
        </div>
        <div>
            <dt>
                {#if help}<AttributeHelp label="Object slots" description={capacitySlotsHelp(profile)} />{:else}Object
                    slots{/if}
            </dt>
            <dd>{capacityMetric(profile.peakSlots, profile.minimumResidentSlots, profile.sharedObjectSlotLimit)}</dd>
        </div>
    </dl>
    {#if help}
        {#each reasons as reason}<p class="capacity-reason">{reason.message}</p>{/each}
    {/if}
</div>

<style>
    .capacity-profile {
        margin-top: 8px;
    }
    .capacity-profile-heading {
        display: flex;
        align-items: baseline;
        justify-content: space-between;
        gap: 8px;
        font-size: 8.5px;
        line-height: 14px;
    }
    h5 {
        margin: 0;
        font: inherit;
        font-weight: 650;
        color: var(--color-text-strong);
    }
    .capacity-profile-heading span {
        white-space: nowrap;
    }
    .failure {
        color: var(--color-danger);
    }
    .capacity-metrics div {
        display: flex;
        flex-wrap: wrap;
        justify-content: space-between;
        gap: 0 8px;
        padding: 3px 0;
    }
    .capacity-metrics dt,
    .capacity-metrics dd {
        flex: 0 0 auto;
        white-space: nowrap;
        overflow-wrap: normal;
    }
    .capacity-reason {
        margin: 3px 0 0;
        font-size: 8.5px;
        line-height: 12px;
        color: var(--color-text-muted);
        overflow-wrap: anywhere;
    }
</style>
