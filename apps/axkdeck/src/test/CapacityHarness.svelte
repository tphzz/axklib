<script lang="ts">
    import { onMount } from 'svelte';
    import TreeNode from '../lib/components/TreeNode.svelte';
    import VolumeInspector from '../lib/components/VolumeInspector.svelte';
    import MutationCapacityDialog from '../lib/components/MutationCapacityDialog.svelte';
    import { provideVolumeCapacity } from '../lib/volumeCapacity.svelte';
    import { provideInspectorPanels } from '../lib/inspectorPanels.svelte';
    import { ASeriesPreferences, provideASeriesPreferences } from '../lib/aSeriesPreferences.svelte';
    import { MutationCapacityWorkflow } from '../features/mutation/capacityWorkflow.svelte';
    import type { CapacityAdmission } from '../lib/importCapacity';
    import type { DiskTreeItem } from '../lib/types';
    import { volumeCapacityFixture } from './volumeCapacityFixture';

    let ready = $state(false);
    let calls = $state(0);
    let checks = $state(0);
    let writes = $state(0);
    let mode = $state('FITS');
    let inspectorMode = $state<'FITS' | 'DOES_NOT_FIT' | 'ERROR'>('FITS');
    let revision = $state(1);
    let outcome = $state('');
    const item: DiskTreeItem = {
        id: 'volume',
        name: 'Capacity Test',
        kind: 'volume',
        partitionIndex: 0,
        childCount: 0,
        sizeBytes: 14365491,
    };
    const preferences = new ASeriesPreferences({ load: async () => 'A4000_A5000', save: async () => {} });
    provideASeriesPreferences(preferences);
    provideInspectorPanels();
    function report(status: string) {
        return {
            baseline: 'FRESH_POWER_ON_WIPE_VOLUME_LOAD',
            partitionIndex: 0,
            volumeDirectoryId: 9,
            volumeName: 'Capacity Test',
            objectCounts: [{ type: 'SBNK', count: 40 }],
            profiles: ['A3000', 'A4000_A5000'].map((target) => ({
                target,
                status,
                parameterByteLimit: target === 'A3000' ? 524288 : 786432,
                sharedObjectSlotLimit: target === 'A3000' ? 1024 : 2048,
                baselineBytes: target === 'A3000' ? 87720 : 111280,
                baselineSlots: target === 'A3000' ? 129 : 130,
                minimumResidentBytes: 123456,
                minimumResidentSlots: 198,
                residentBytes: status === 'FITS' ? 123456 : null,
                peakBytes: status === 'FITS' ? 124000 : null,
                peakSlots: status === 'FITS' ? 198 : null,
                reasons:
                    status === 'FITS'
                        ? []
                        : [{ code: 'LOAD_ALLOCATION_FAILED', message: 'Parameter memory limit exceeded.' }],
            })),
        } as CapacityAdmission['reports'][number];
    }
    const capacity = provideVolumeCapacity(() => ({
        sessionId: 1,
        revision,
        enabled: true,
        transport: {
            inspectVolumeCapacity: async (_session, contentScopeId) => {
                calls++;
                const currentRevision = revision;
                const status = inspectorMode;
                await new Promise((resolve) => setTimeout(resolve, 80));
                if (status === 'ERROR') throw new Error('Capacity service disconnected');
                return {
                    imageId: 'image',
                    revision: currentRevision,
                    contentScopeId,
                    report: volumeCapacityFixture(status),
                };
            },
        },
    }));
    const workflow = new MutationCapacityWorkflow(preferences);
    async function apply() {
        outcome = '';
        try {
            await workflow.review(async (policy) => {
                checks++;
                if (mode === 'ERROR') throw new Error('Capacity service disconnected');
                return {
                    target: policy.target,
                    reports: Array.from({ length: 20 }, (_, i) => ({ ...report(mode), volumeName: `Volume ${i + 1}` })),
                    allowed: mode === 'FITS',
                };
            });
            writes++;
            outcome = 'Written';
        } catch (error) {
            outcome = error instanceof Error ? error.message : 'Failed';
        }
    }
    onMount(() => {
        void preferences.ready.then(() => (ready = true));
        return () => workflow.dispose();
    });
</script>

{#if ready}
    <nav aria-label="Capacity fixture controls">
        <select class="dialog-field-control" aria-label="Capacity scenario" bind:value={mode}>
            <option value="FITS">Fits</option><option value="DOES_NOT_FIT">Does not fit</option><option value="ERROR"
                >Error</option
            >
        </select>
        <button class="secondary-button" onclick={() => void apply()}>Apply change</button>
        <button class="secondary-button" onclick={() => capacity.showObject()}>Show object</button>
        <select
            class="dialog-field-control"
            aria-label="Inspector scenario"
            bind:value={inspectorMode}
            onchange={() => revision++}
        >
            <option value="FITS">Fits</option><option value="DOES_NOT_FIT">Does not fit</option><option value="ERROR"
                >Error</option
            >
        </select>
    </nav>
    <main class="fixture-layout">
        <section class="background-pane" data-background-pane>
            <TreeNode
                {item}
                selectedId=""
                selectedVolumeIds={capacity.selectedVolume ? [item.id] : []}
                onselect={() => capacity.selectVolume(item)}
                onloadchildren={async () => ({ items: [], totalCount: 0 })}
            />
            {#each Array.from({ length: 200 }, (_, i) => i) as i}<p>Background row {i + 1}</p>{/each}
        </section>
        {#if capacity.selectedVolume}<VolumeInspector {item} />{:else}<section class="object-placeholder">
                Stored format a3k
            </section>{/if}
    </main>
    <output data-capacity-state>{JSON.stringify({ calls, checks, writes, outcome })}</output>
    <MutationCapacityDialog {workflow} />
{/if}

<style>
    :global(body) {
        margin: 0;
        background: var(--color-bg);
    }
    nav {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
        padding: 12px;
    }
    .fixture-layout {
        display: grid;
        grid-template-columns: minmax(0, 1fr) clamp(160px, 40%, 300px);
        height: 520px;
    }
    .background-pane {
        overflow: auto;
        min-height: 0;
    }
    :global(.fixture-layout > .inspector) {
        grid-column: 2;
        grid-row: 1;
        min-width: 0;
    }
    .background-pane p {
        margin: 0;
        padding: 8px;
        border-bottom: 1px solid var(--color-border);
    }
    .object-placeholder {
        padding: 12px;
    }
    output {
        display: block;
        padding: 12px;
        overflow-wrap: anywhere;
    }
</style>
