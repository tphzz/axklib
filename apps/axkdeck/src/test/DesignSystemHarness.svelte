<script lang="ts">
    import CollectionRow from '../lib/components/CollectionRow.svelte';
    import CollectionIdentity from '../lib/components/CollectionIdentity.svelte';
    import SwitchControl from '../lib/components/SwitchControl.svelte';
    import AuditionBar from '../lib/components/AuditionBar.svelte';
    import PlacementRepairDialog from '../lib/components/PlacementRepairDialog.svelte';
    import AllocationGrid from '../lib/components/AllocationGrid.svelte';
    import { allocationLegendOrder, allocationPalette, type AllocationRun } from '../lib/allocationGrid';
    import type { components } from '../lib/generated/axklibApiV1';
    import '../features/object-editor/editor.css';

    let checked = $state(false);
    let changes = $state(0);
    let autoplay = $state(false);
    let open = $state(false);
    let scenario = $state('idle');
    let submissions = $state(0);
    let cellSize = $state(7);
    const busy = $derived(scenario === 'inspecting' || scenario === 'repairing');
    const inspection = {
        imageId: 'fixture',
        revision: 1,
        scope: { kind: 'PARTITION' as const, partitionIndex: 0 },
        canRepair: true,
        repairObjectCount: 62,
        blockedObjectCount: 2,
        recoveryVolumeName: 'Recovered',
        destinations: [
            { volumeName: 'Recovered', createsVolume: true, objectCount: 62, objectTypeCounts: { SMPL: 62 } },
        ],
        blockers: [{ code: 'AMBIGUOUS_OWNER', count: 2, message: 'Ambiguous objects remain unchanged.' }],
    };
    const details = 'Retained result: ' + 'Objects with ambiguous ownership remain unchanged. '.repeat(35);
    const kinds: AllocationRun['allocationKind'][] = [
        'RESERVED',
        'CONTINUATION',
        'DIRECTORY',
        'SUPPORT',
        'DATA',
        'UNKNOWN',
        'FREE',
        'UNCLAIMED',
        'DATA',
        'DATA',
        'CONFLICT',
    ];
    const flags: AllocationRun['consistencyFlags'][] = [
        [],
        [],
        [],
        [],
        [],
        [],
        [],
        ['USED_WITHOUT_CLAIM'],
        ['BITMAP_COPY_MISMATCH'],
        ['CLAIMED_BUT_FREE'],
        ['MULTIPLE_CLAIMS'],
    ];
    const runs: AllocationRun[] = allocationLegendOrder.map((_, index) => ({
        startCluster: index,
        clusterCount: 1,
        allocationKind: kinds[index]!,
        consistencyFlags: flags[index]!,
        bitmapCopy1Used: true,
        bitmapCopy2Used: true,
        reconstructedUsed: true,
        owners: [],
        byteOffset: index * 4096,
        byteCount: 4096,
        startSector: index * 8,
        sectorCount: 8,
    }));
    const map = {
        clusterCount: 65536,
        clusterSizeBytes: 4096,
        sectorsPerCluster: 8,
        partitionIndex: 0,
        revision: 1,
        runs: [...runs, { ...runs[6], startCluster: 11, clusterCount: 65525 }],
    } as components['schemas']['ImageAllocationMap'];
    function toggle(next: boolean) {
        checked = next;
        changes++;
    }
    function repair(next: string) {
        scenario = next;
        open = true;
    }
    Object.assign(window, {
        designFixture: {
            finish: () => {
                scenario = 'idle';
            },
        },
    });
</script>

<main class="design-fixture">
    <div class="brand"><strong>axkdeck</strong></div>
    <section aria-label="Actions" class="dialog-shell">
        <footer class="dialog-footer">
            <span class="dialog-footer-status">Ready</span>
            <div class="dialog-footer-actions">
                <button class="secondary-button">Cancel</button>
                <button class="primary-button">Create</button>
                <button class="danger-button">Delete</button>
            </div>
        </footer>
    </section>
    <section class="controls" aria-label="Fields and states">
        <label>Volume<input class="dialog-field-control" placeholder="Volume name" /></label>
        <div class="warning">Warning<span>Unknown ownership</span></div>
        <div class="dialog-segmented-control"><button aria-pressed="true">Image</button><button>Device</button></div>
        <label><input class="dialog-checkbox" type="checkbox" checked />Dialog mark</label>
    </section>
    <section aria-label="Collection states">
        <CollectionRow data-fixture="default" slotLabel="001"
            ><CollectionIdentity name="Default sample" metadata="31 samples - 912 B" /></CollectionRow
        >
        <CollectionRow data-fixture="active" active slotLabel="002"
            ><CollectionIdentity name="Active sample" metadata="31 samples - 912 B" /></CollectionRow
        >
        <CollectionRow data-fixture="selected" selected slotLabel="003"
            ><CollectionIdentity name="Selected sample" metadata="31 samples - 912 B" /></CollectionRow
        >
        <button class="sequence-row selected">Sequence<span>Selected sequence metadata</span></button>
        <button class="storage-picker-row storage-picker-row-active"
            ><span>Keyboard-active file</span><small>Read-only source</small></button
        >
        <button class="tree-row selected" style:--tree-depth={0}>Selected volume</button>
    </section>
    <section class="controls" aria-label="Switches">
        <div class="device-editor"><SwitchControl label="Editor switch" {checked} onchange={toggle} /></div>
        <div class="log-viewer">
            <SwitchControl label="Diagnostic switch" {checked} density="diagnostic" showLabel onchange={toggle} />
        </div>
        <fieldset disabled><SwitchControl label="Inherited disabled switch" {checked} onchange={toggle} /></fieldset>
        <SwitchControl label="Explicit disabled switch" {checked} disabled onchange={toggle} />
        <output aria-label="Switch changes">{changes}</output>
    </section>
    <AuditionBar
        available
        {autoplay}
        state={{ objectId: null, status: 'idle', playheadFrame: 0 }}
        label=""
        onautoplaychange={(next) => {
            autoplay = next;
        }}
    />
    <section aria-label="Allocation">
        <div class="controls">
            {#each [4, 7, 11] as size}<button
                    onclick={() => {
                        cellSize = size;
                    }}>{size} pixel cells</button
                >{/each}
        </div>
        <div class="legend">
            {#each allocationLegendOrder as kind}<span
                    ><i data-kind={kind} style:background={allocationPalette[kind].color}></i>{allocationPalette[kind]
                        .label}</span
                >{/each}
        </div>
        <div class="allocation"><AllocationGrid {map} {cellSize} /></div>
    </section>
    <section class="controls" aria-label="Repair states">
        {#each ['idle', 'inspecting', 'repairing', 'error', 'success', 'blocked'] as state}<button
                onclick={() => repair(state)}>Repair {state}</button
            >{/each}
        <output aria-label="Repair submissions">{submissions}</output>
    </section>
</main>
{#if open}
    <PlacementRepairDialog
        item={{ id: 'partition', name: 'Single-Cycle', kind: 'partition', partitionIndex: 0, childCount: 21 }}
        inspection={scenario === 'inspecting' ? null : { ...inspection, canRepair: scenario !== 'blocked' }}
        {busy}
        phase={scenario === 'inspecting' || scenario === 'repairing' ? scenario : 'idle'}
        error={scenario === 'error' ? details : ''}
        message={scenario === 'success' ? details : ''}
        oncancel={() => {
            open = false;
        }}
        onsubmit={() => {
            submissions++;
            scenario = 'repairing';
        }}
    />
{/if}

<style>
    .design-fixture {
        height: 100vh;
        overflow: auto;
        padding: 12px;
        display: grid;
        align-content: start;
        gap: 12px;
        font-size: 11px;
    }
    .brand {
        padding: 0;
    }
    section {
        min-width: 0;
    }
    .controls {
        display: flex;
        align-items: center;
        flex-wrap: wrap;
        gap: 8px;
    }
    label {
        display: grid;
        gap: 4px;
    }
    fieldset {
        border: 0;
        margin: 0;
        padding: 0;
    }
    .warning {
        color: var(--color-warning);
    }
    .dialog-shell {
        width: 100%;
    }
    .controls .log-viewer {
        display: flex;
        height: auto;
    }
    .controls > .device-editor {
        flex: 0 0 30px;
        width: 30px;
        height: 17px;
    }
    .storage-picker-row small {
        color: var(--color-text-muted);
    }
    .legend {
        display: flex;
        flex-wrap: wrap;
        gap: 6px;
        margin-block: 8px;
    }
    .legend span {
        display: inline-flex;
        align-items: center;
        gap: 4px;
    }
    .legend i {
        display: inline-block;
        width: 9px;
        height: 9px;
    }
    .allocation {
        height: 150px;
    }
</style>
