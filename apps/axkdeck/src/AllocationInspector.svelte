<script lang="ts">
    import { onMount } from 'svelte';
    import AllocationGrid from './lib/components/AllocationGrid.svelte';
    import Icon from './lib/components/Icon.svelte';
    import type { components } from './lib/generated/axklibApiV1';
    import { AxklibHttpApiClient } from './lib/httpApiClient';
    import {
        allocationExportFilename,
        allocationSpaceStatistic,
        formatAllocationBytes,
        resolveAllocationServerConnection,
        saveAllocationMap,
    } from './lib/allocationInspector';

    type AllocationMap = components['schemas']['ImageAllocationMap'];

    const query = new URLSearchParams(window.location.search);
    const imageId = query.get('imageId') ?? '';
    const revision = Number(query.get('revision'));
    const partitionIndex = Number(query.get('partitionIndex'));
    const requestedPartitionName = query.get('partitionName') ?? '';

    let map = $state<AllocationMap | null>(null);
    let error = $state('');
    let exportError = $state('');
    let exporting = $state(false);
    let cellSize = $state(7);

    const freeSpace = $derived(map ? allocationSpaceStatistic(map.summary.freeClusters, map.clusterSizeBytes) : null);
    const largestFreeRun = $derived(
        map ? allocationSpaceStatistic(map.summary.largestFreeRunClusters, map.clusterSizeBytes) : null,
    );

    const anomalyCount = $derived(
        map
            ? map.summary.bitmapCopyMismatchClusters +
                  map.summary.claimedButFreeClusters +
                  map.summary.usedWithoutClaimClusters +
                  map.summary.conflictingClusters +
                  map.summary.invalidExtentRecords
            : 0,
    );

    function describeError(reason: unknown): string {
        const message = reason instanceof Error ? reason.message : String(reason);
        if (/revision/i.test(message)) {
            return 'The image changed after this inspector was opened. Close it and reopen the allocation view.';
        }
        return message;
    }

    async function exportJson(): Promise<void> {
        if (!map || exporting) return;
        exportError = '';
        exporting = true;
        try {
            await saveAllocationMap(allocationExportFilename(map.partitionName, map.partitionIndex), map);
        } catch (reason) {
            exportError = describeError(reason);
        } finally {
            exporting = false;
        }
    }

    onMount(async () => {
        if (
            !imageId ||
            !Number.isSafeInteger(revision) ||
            revision < 0 ||
            !Number.isSafeInteger(partitionIndex) ||
            partitionIndex < 0
        ) {
            error = 'This allocation inspector link is incomplete.';
            return;
        }
        try {
            const connection = await resolveAllocationServerConnection();
            if (!connection) {
                error = 'No axklib-server connection is available.';
                return;
            }
            const client = new AxklibHttpApiClient(connection);
            const parameters = new URLSearchParams({
                partitionIndex: String(partitionIndex),
                expectedRevision: String(revision),
            });
            map = await client.request<AllocationMap>(
                'GET',
                `/images/${encodeURIComponent(imageId)}/allocation-map?${parameters}`,
            );
        } catch (reason) {
            error = describeError(reason);
        }
    });
</script>

<main class="allocation-inspector">
    <header>
        <div>
            <span class="eyebrow">Partition allocation</span>
            <h1>{map?.partitionName || requestedPartitionName || `Partition ${partitionIndex + 1}`}</h1>
        </div>
        <div class="header-actions">
            {#if exportError}<span class="export-error" role="alert" title={exportError}>{exportError}</span>{/if}
            <div class="cell-size" aria-label="Cluster cell size">
                {#each [4, 7, 11] as size}
                    <button
                        class:active={cellSize === size}
                        onclick={() => (cellSize = size)}
                        title={`${size} pixel cells`}
                    >
                        {size}
                    </button>
                {/each}
            </div>
            <button
                class="export-action"
                onclick={exportJson}
                disabled={!map || exporting}
                title="Export allocation map as JSON"
            >
                <Icon name="save" size={15} />
                {exporting ? 'Saving…' : 'Export JSON'}
            </button>
        </div>
    </header>

    {#if map}
        <section class="statistics" aria-label="Partition allocation statistics">
            <div><span>Clusters</span><strong>{map.summary.totalClusters.toLocaleString()}</strong></div>
            <div><span>Allocated</span><strong>{map.summary.allocatedClusters.toLocaleString()}</strong></div>
            <div>
                <span>Free</span><strong>{freeSpace?.primary}</strong><small>{freeSpace?.secondary}</small>
            </div>
            <div>
                <span>Largest free run</span><strong>{largestFreeRun?.primary}</strong><small
                    >{largestFreeRun?.secondary}</small
                >
            </div>
            <div>
                <span>Records / extents</span><strong
                    >{map.summary.recordCount.toLocaleString()} / {map.summary.totalExtentCount.toLocaleString()}</strong
                >
            </div>
            <div>
                <span>Fragmented records</span><strong>{map.summary.fragmentedRecordCount.toLocaleString()}</strong>
            </div>
            <div>
                <span>Logical / allocated</span><strong
                    >{formatAllocationBytes(map.summary.logicalRecordBytes)} / {formatAllocationBytes(
                        map.summary.allocatedBytes,
                    )}</strong
                >
            </div>
            <div>
                <span title="Unused bytes inside allocated clusters; unavailable for new allocations."
                    >Allocated slack</span
                ><strong>{formatAllocationBytes(map.summary.dataSlackBytes)}</strong><small>Not free space</small>
            </div>
            <div class:warning={anomalyCount > 0}>
                <span>Integrity findings</span><strong>{anomalyCount.toLocaleString()}</strong>
            </div>
        </section>
        <section class="legend" aria-label="Allocation legend">
            <span><i class="reserved"></i>Reserved</span>
            <span><i class="continuation"></i>Record continuation</span>
            <span><i class="directory"></i>Directory data</span>
            <span><i class="support"></i>SFS support data</span>
            <span><i class="data"></i>Object data</span>
            <span><i class="unknown"></i>Unknown record data</span>
            <span><i class="free"></i>Free</span>
            <span><i class="unclaimed"></i>Allocated without owner</span>
            <span><i class="mismatch"></i>Bitmap copies differ</span>
            <span><i class="claim-mismatch"></i>Index claim, bitmap free</span>
            <span><i class="conflict"></i>Multiple claims</span>
        </section>
        <section class="map-region">
            <AllocationGrid {map} {cellSize} />
        </section>
        <footer>
            <span>Partition {map.partitionIndex + 1} · revision {map.revision}</span>
            <span
                >{map.clusterSizeBytes.toLocaleString()} bytes per cluster · {map.sectorsPerCluster} sectors per cluster</span
            >
        </footer>
    {:else if error}
        <section class="message error-message">
            <Icon name="triangle-alert" size={18} />
            <div><strong>Allocation map unavailable</strong><span>{error}</span></div>
        </section>
    {:else}
        <section class="message"><span>Reading allocation metadata…</span></section>
    {/if}
</main>

<style>
    .allocation-inspector {
        display: grid;
        grid-template-rows: auto auto auto minmax(0, 1fr) auto;
        width: 100%;
        height: 100%;
        min-width: 720px;
        color: var(--color-text);
        background: var(--color-bg);
    }
    header {
        display: flex;
        min-height: 54px;
        align-items: center;
        justify-content: space-between;
        gap: 16px;
        padding: 7px 14px;
        border-bottom: 1px solid var(--color-border);
        background: var(--color-panel);
    }
    .eyebrow {
        color: var(--color-text-muted);
        font-size: 9px;
        text-transform: uppercase;
    }
    h1 {
        max-width: 60vw;
        margin: 1px 0 0;
        overflow: hidden;
        color: var(--color-text-strong);
        font-size: 15px;
        letter-spacing: 0;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .header-actions,
    .cell-size,
    .export-action {
        display: flex;
        align-items: center;
    }
    .header-actions {
        gap: 8px;
    }
    .export-error {
        max-width: 320px;
        overflow: hidden;
        color: var(--color-danger);
        font-size: 10px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .cell-size {
        overflow: hidden;
        border: 1px solid var(--color-border);
        border-radius: 5px;
    }
    .cell-size button,
    .export-action {
        height: 28px;
        border: 0;
        background: transparent;
        cursor: pointer;
    }
    .cell-size button {
        width: 30px;
        color: var(--color-text-muted);
        border-right: 1px solid var(--color-border);
        font-size: 10px;
    }
    .cell-size button:last-child {
        border-right: 0;
    }
    .cell-size button.active {
        color: var(--color-text-strong);
        background: var(--color-accent-strong);
    }
    .export-action {
        gap: 6px;
        padding: 0 9px;
        color: var(--color-text-strong);
        border: 1px solid var(--color-border);
        border-radius: 5px;
    }
    .export-action:disabled {
        opacity: 0.45;
        cursor: default;
    }
    .statistics {
        display: grid;
        grid-template-columns: repeat(9, minmax(90px, 1fr));
        border-bottom: 1px solid var(--color-border);
        background: #171a1c;
    }
    .statistics div {
        display: grid;
        min-width: 0;
        gap: 2px;
        padding: 7px 10px;
        border-right: 1px solid #303639;
    }
    .statistics div:last-child {
        border-right: 0;
    }
    .statistics span {
        color: var(--color-text-muted);
        font-size: 9px;
    }
    .statistics strong {
        overflow: hidden;
        color: var(--color-text-strong);
        font-size: 11px;
        font-weight: 600;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .statistics small {
        overflow: hidden;
        color: var(--color-text-muted);
        font-size: 8px;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .statistics .warning strong {
        color: var(--color-danger);
    }
    .legend {
        display: flex;
        min-height: 31px;
        align-items: center;
        flex-wrap: wrap;
        gap: 6px 16px;
        padding: 5px 12px;
        border-bottom: 1px solid var(--color-border);
        font-size: 9px;
    }
    .legend span {
        display: inline-flex;
        align-items: center;
        gap: 5px;
    }
    .legend i {
        width: 9px;
        height: 9px;
        border: 1px solid rgb(255 255 255 / 18%);
    }
    .reserved {
        background: #718096;
    }
    .continuation {
        background: #fbbf24;
    }
    .directory {
        background: #2dd4bf;
    }
    .support {
        background: #94a3b8;
    }
    .data {
        background: #34d399;
    }
    .unknown {
        background: #f97316;
    }
    .free {
        background: #252b2e;
    }
    .unclaimed {
        background: #60a5fa;
    }
    .mismatch {
        background: #e879f9;
    }
    .claim-mismatch {
        background: #fb7185;
    }
    .conflict {
        background: #ef4444;
    }
    .map-region {
        min-height: 0;
        padding: 8px 10px;
    }
    footer {
        display: flex;
        min-height: 24px;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
        padding: 0 10px;
        color: var(--color-text-muted);
        border-top: 1px solid var(--color-border);
        font-size: 9px;
    }
    .message {
        display: flex;
        grid-row: 2 / -1;
        align-items: center;
        justify-content: center;
        gap: 9px;
        color: var(--color-text-muted);
    }
    .error-message {
        color: var(--color-danger);
    }
    .error-message div {
        display: grid;
        gap: 3px;
    }
    .error-message span {
        color: var(--color-text);
    }
    @media (max-width: 1050px) {
        .statistics {
            grid-template-columns: repeat(5, minmax(100px, 1fr));
        }
        .statistics div {
            border-bottom: 1px solid #303639;
        }
    }
</style>
