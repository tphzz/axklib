<script lang="ts">
    import type { VolumeCapacityState } from '../volumeCapacity.svelte';
    import VolumeCapacityProfile from './VolumeCapacityProfile.svelte';
    let { state }: { state: VolumeCapacityState | undefined } = $props();
</script>

{#if state?.status === 'ready'}
    {#each state.inspection.report.profiles as profile (profile.target)}
        <VolumeCapacityProfile {profile} />
    {/each}
{:else if state?.status === 'error'}<span>Capacity unavailable: {state.message}</span>
{:else}<span>Inspecting sampler capacity...</span>{/if}
