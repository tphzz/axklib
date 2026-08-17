<script lang="ts">
    import { onMount } from 'svelte';
    import type { components } from '../generated/axklibApiV1';
    import {
        allocationGridCluster,
        allocationGridColumns,
        allocationConsistencyLabel,
        allocationRunVisualKind,
        findAllocationRun,
        type AllocationRun,
    } from '../allocationGrid';

    type AllocationMap = components['schemas']['ImageAllocationMap'];

    interface Props {
        map: AllocationMap;
        cellSize: number;
    }

    let { map, cellSize }: Props = $props();
    let viewport: HTMLDivElement;
    let canvas: HTMLCanvasElement;
    let width = $state(1);
    let scrollTop = $state(0);
    let viewportHeight = $state(1);
    let hovered = $state<{ cluster: number; run: AllocationRun; x: number; y: number } | null>(null);
    const gap = 1;
    const columns = $derived(allocationGridColumns(width, cellSize, gap));
    const rows = $derived(Math.ceil(map.clusterCount / columns));
    const totalHeight = $derived(rows * (cellSize + gap));
    const firstRow = $derived(Math.max(0, Math.floor(scrollTop / (cellSize + gap)) - 2));
    const visibleRows = $derived(Math.ceil(viewportHeight / (cellSize + gap)) + 4);
    const canvasTop = $derived(firstRow * (cellSize + gap));
    const canvasHeight = $derived(Math.min(totalHeight - canvasTop, visibleRows * (cellSize + gap)));

    function runColor(run: AllocationRun): string {
        switch (allocationRunVisualKind(run)) {
            case 'multiple-claims':
                return '#ef4444';
            case 'claimed-but-free':
                return '#fb7185';
            case 'bitmap-mismatch':
                return '#e879f9';
            case 'used-without-claim':
                return '#60a5fa';
            case 'reserved':
                return '#718096';
            case 'continuation':
                return '#fbbf24';
            case 'directory':
                return '#2dd4bf';
            case 'support':
                return '#94a3b8';
            case 'data':
                return '#34d399';
            case 'unknown':
                return '#f97316';
            default:
                return '#252b2e';
        }
    }

    function draw(): void {
        if (!canvas || canvasHeight <= 0) return;
        const scale = window.devicePixelRatio || 1;
        const cssHeight = Math.max(1, canvasHeight);
        canvas.width = Math.max(1, Math.floor(width * scale));
        canvas.height = Math.max(1, Math.floor(cssHeight * scale));
        canvas.style.width = `${width}px`;
        canvas.style.height = `${cssHeight}px`;
        const context = canvas.getContext('2d');
        if (!context) return;
        context.scale(scale, scale);
        context.clearRect(0, 0, width, cssHeight);
        const startCluster = firstRow * columns;
        const endCluster = Math.min(map.clusterCount, (firstRow + visibleRows) * columns);
        for (let cluster = startCluster; cluster < endCluster; cluster += 1) {
            const run = findAllocationRun(map.runs, cluster);
            if (!run) continue;
            const localRow = Math.floor(cluster / columns) - firstRow;
            const column = cluster % columns;
            context.fillStyle = runColor(run);
            context.fillRect(column * (cellSize + gap), localRow * (cellSize + gap), cellSize, cellSize);
        }
        if (hovered && hovered.cluster >= startCluster && hovered.cluster < endCluster) {
            const localRow = Math.floor(hovered.cluster / columns) - firstRow;
            const column = hovered.cluster % columns;
            context.strokeStyle = '#ffffff';
            context.lineWidth = 1;
            context.strokeRect(
                column * (cellSize + gap) + 0.5,
                localRow * (cellSize + gap) + 0.5,
                Math.max(1, cellSize - 1),
                Math.max(1, cellSize - 1),
            );
        }
    }

    function resize(): void {
        width = viewport.clientWidth;
        viewportHeight = viewport.clientHeight;
    }

    function updateScroll(): void {
        scrollTop = viewport.scrollTop;
    }

    function inspect(event: MouseEvent): void {
        const bounds = canvas.getBoundingClientRect();
        const cluster = allocationGridCluster(
            event.clientX - bounds.left,
            event.clientY - bounds.top,
            columns,
            firstRow,
            cellSize,
            gap,
        );
        const run = cluster === null || cluster >= map.clusterCount ? null : findAllocationRun(map.runs, cluster);
        hovered = run ? { cluster: cluster!, run, x: event.clientX, y: event.clientY } : null;
    }

    $effect(() => {
        width;
        scrollTop;
        viewportHeight;
        cellSize;
        hovered?.cluster;
        draw();
    });

    onMount(() => {
        const observer = new ResizeObserver(resize);
        observer.observe(viewport);
        resize();
        return () => observer.disconnect();
    });
</script>

<div class="allocation-viewport" bind:this={viewport} onscroll={updateScroll} aria-label="Partition cluster map">
    <div class="allocation-surface" style:height={`${totalHeight}px`}>
        <canvas
            bind:this={canvas}
            style:top={`${canvasTop}px`}
            onmousemove={inspect}
            onmouseleave={() => (hovered = null)}
        ></canvas>
    </div>
</div>

{#if hovered}
    <div
        class="allocation-tooltip"
        style:left={`${Math.min(hovered.x + 14, window.innerWidth - 370)}px`}
        style:top={`${Math.min(hovered.y + 14, window.innerHeight - 260)}px`}
    >
        <strong>Partition cluster {hovered.cluster.toLocaleString()}</strong>
        <span
            >{hovered.run.allocationKind.toLocaleLowerCase()} · sector {(
                map.partitionStartSector +
                hovered.cluster * map.sectorsPerCluster
            ).toLocaleString()}</span
        >
        <span
            >Byte {(
                map.partitionStartSector * map.sectorSizeBytes +
                hovered.cluster * map.clusterSizeBytes
            ).toLocaleString()}</span
        >
        {#each hovered.run.owners as owner}
            <div class="owner">
                <strong>{owner.objectName || owner.objectType || owner.recordKind}</strong>
                <span>{owner.objectType || owner.recordKind}{owner.volumeName ? ` · ${owner.volumeName}` : ''}</span>
                <span
                    >{owner.sfsId === null
                        ? owner.claimKind
                        : `SFS ${owner.sfsId} · ${owner.claimKind}${owner.extentIndex === null ? '' : ` ${owner.extentIndex + 1}`}`}</span
                >
            </div>
        {/each}
        {#each hovered.run.consistencyFlags as flag}<span class="problem">{allocationConsistencyLabel(flag)}</span
            >{/each}
    </div>
{/if}

<style>
    .allocation-viewport {
        min-height: 0;
        height: 100%;
        overflow-y: auto;
        background: #111416;
        border: 1px solid var(--line, #384044);
    }
    .allocation-surface {
        position: relative;
        width: 100%;
    }
    canvas {
        position: absolute;
        left: 0;
        cursor: crosshair;
    }
    .allocation-tooltip {
        position: fixed;
        z-index: 100;
        display: grid;
        width: min(350px, calc(100vw - 24px));
        gap: 3px;
        padding: 10px 12px;
        color: #e8edef;
        background: #202527;
        border: 1px solid #566166;
        box-shadow: 0 8px 24px rgb(0 0 0 / 45%);
        pointer-events: none;
        font-size: 12px;
    }
    .allocation-tooltip > span,
    .owner span {
        color: #9eabb1;
    }
    .owner {
        display: grid;
        gap: 2px;
        margin-top: 6px;
        padding-top: 6px;
        border-top: 1px solid #3a4246;
    }
    .problem {
        color: #fca5a5 !important;
    }
</style>
