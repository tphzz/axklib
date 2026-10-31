<script lang="ts">
    import { onMount, tick } from 'svelte';
    import { on } from 'svelte/events';
    import Icon from './Icon.svelte';
    import type { components } from '../generated/axklibApiV1';
    import {
        allocationGridCluster,
        allocationGridColumns,
        allocationConsistencyLabel,
        allocationRunVisualKind,
        allocationPalette,
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
    let pinned = $state(false);
    let currentCluster = $state(0);
    let details: HTMLElement | undefined;
    let restoringFocus = false;
    const detailsId = $props.id();
    const gap = 1;
    const columns = $derived(allocationGridColumns(width, cellSize, gap));
    const rows = $derived(Math.ceil(map.clusterCount / columns));
    const totalHeight = $derived(rows * (cellSize + gap));
    const firstRow = $derived(Math.max(0, Math.floor(scrollTop / (cellSize + gap)) - 2));
    const visibleRows = $derived(Math.ceil(viewportHeight / (cellSize + gap)) + 4);
    const canvasTop = $derived(firstRow * (cellSize + gap));
    const canvasHeight = $derived(Math.min(totalHeight - canvasTop, visibleRows * (cellSize + gap)));

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
            context.fillStyle = allocationPalette[allocationRunVisualKind(run)].color;
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

    function inspect(event: MouseEvent, force = false): void {
        if (pinned || (!force && document.activeElement === canvas)) return;
        const bounds = canvas.getBoundingClientRect();
        const scaleX = width ? bounds.width / width : 1;
        const scaleY = canvasHeight ? bounds.height / canvasHeight : 1;
        const cluster = allocationGridCluster(
            (event.clientX - bounds.left) / (scaleX || 1),
            (event.clientY - bounds.top) / (scaleY || 1),
            columns,
            firstRow,
            cellSize,
            gap,
        );
        const run = cluster === null || cluster >= map.clusterCount ? null : findAllocationRun(map.runs, cluster);
        hovered = run ? { cluster: cluster!, run, x: event.clientX, y: event.clientY } : null;
        if (hovered) currentCluster = hovered.cluster;
    }

    function closeDetails(restore = false): void {
        hovered = null;
        pinned = false;
        if (restore) {
            restoringFocus = true;
            canvas.focus({ preventScroll: true });
            restoringFocus = false;
        }
    }

    function inspectCluster(cluster: number, reveal = true): void {
        if (!map.clusterCount) return;
        currentCluster = Math.max(0, Math.min(map.clusterCount - 1, cluster));
        const row = Math.floor(currentCluster / columns);
        const top = row * (cellSize + gap);
        if (reveal && top < viewport.scrollTop) viewport.scrollTop = top;
        else if (reveal && top + cellSize > viewport.scrollTop + viewportHeight)
            viewport.scrollTop = top + cellSize - viewportHeight;
        updateScroll();
        if (!reveal && (top + cellSize < scrollTop || top > scrollTop + viewportHeight)) {
            closeDetails();
            return;
        }
        const run = findAllocationRun(map.runs, currentCluster);
        const bounds = viewport.getBoundingClientRect();
        const scale = width ? bounds.width / width : 1;
        hovered = run
            ? {
                  cluster: currentCluster,
                  run,
                  x: bounds.left + (currentCluster % columns) * (cellSize + gap) * scale,
                  y: bounds.top + (top - viewport.scrollTop) * scale,
              }
            : null;
    }

    async function pinDetails(): Promise<void> {
        if (!hovered) inspectCluster(currentCluster);
        if (!hovered) return;
        pinned = true;
        await tick();
        details?.focus({ preventScroll: true });
    }

    function navigate(event: KeyboardEvent): void {
        const page = Math.max(1, Math.floor(viewportHeight / (cellSize + gap))) * columns;
        const next = {
            ArrowLeft: currentCluster - 1,
            ArrowRight: currentCluster + 1,
            ArrowUp: currentCluster - columns,
            ArrowDown: currentCluster + columns,
            Home: 0,
            End: map.clusterCount - 1,
            PageUp: currentCluster - page,
            PageDown: currentCluster + page,
        }[event.key];
        if (next !== undefined) {
            event.preventDefault();
            inspectCluster(next);
        } else if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            void pinDetails();
        }
    }

    function mountDetails(node: HTMLElement, _anchor: typeof hovered) {
        details = node;
        document.body.append(node);
        function position() {
            if (!hovered) return;
            const scale = canvas.offsetWidth ? canvas.getBoundingClientRect().width / canvas.offsetWidth : 1;
            const bodyScale = document.body.offsetWidth
                ? document.body.getBoundingClientRect().width / document.body.offsetWidth
                : 1;
            node.style.zoom = String(scale / (bodyScale || 1));
            node.style.width = `${Math.min(350, (window.innerWidth - 16) / (scale || 1))}px`;
            node.style.maxHeight = `${(window.innerHeight - 16) / (scale || 1)}px`;
            const bounds = node.getBoundingClientRect();
            node.style.left = `${Math.max(8, Math.min(hovered.x + 14, window.innerWidth - bounds.width - 8)) / (scale || 1)}px`;
            node.style.top = `${Math.max(8, Math.min(hovered.y + 14, window.innerHeight - bounds.height - 8)) / (scale || 1)}px`;
        }
        position();
        const observer = new ResizeObserver(position);
        observer.observe(node);
        const release = on(window, 'resize', () => closeDetails());
        return {
            update: position,
            destroy() {
                observer.disconnect();
                release();
                node.remove();
                details = undefined;
            },
        };
    }

    $effect(() => {
        map;
        cellSize;
        closeDetails();
        currentCluster = 0;
    });

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
        const outside = (target: EventTarget | null) =>
            target instanceof Node && target !== canvas && !details?.contains(target);
        const release = [
            on(
                window,
                'pointerdown',
                (event) => {
                    if (outside(event.target)) closeDetails();
                },
                { capture: true },
            ),
            on(window, 'focusin', (event) => {
                if (outside(event.target)) closeDetails();
            }),
            on(
                window,
                'keydown',
                (event) => {
                    if (event.key === 'Escape' && hovered) {
                        event.preventDefault();
                        event.stopPropagation();
                        closeDetails(!!details?.contains(document.activeElement));
                    }
                },
                { capture: true },
            ),
            on(
                window,
                'scroll',
                (event) => {
                    if (event.target instanceof Node && details?.contains(event.target)) return;
                    if (document.activeElement === canvas) inspectCluster(currentCluster, false);
                    else closeDetails();
                },
                { capture: true },
            ),
        ];
        return () => {
            observer.disconnect();
            release.forEach((remove) => remove());
        };
    });
</script>

<div class="allocation-viewport" bind:this={viewport} onscroll={updateScroll} aria-label="Partition cluster map">
    <div class="allocation-surface" style:height={`${totalHeight}px`}>
        <canvas
            bind:this={canvas}
            style:top={`${canvasTop}px`}
            onmousemove={inspect}
            onmouseleave={() => {
                if (!pinned && document.activeElement !== canvas) closeDetails();
            }}
            role="slider"
            aria-label="Partition cluster"
            aria-valuemin={0}
            aria-valuemax={Math.max(0, map.clusterCount - 1)}
            aria-valuenow={currentCluster}
            aria-valuetext={`Cluster ${currentCluster.toLocaleString()}`}
            aria-disabled={!map.clusterCount}
            aria-controls={hovered ? detailsId : undefined}
            tabindex={map.clusterCount ? 0 : -1}
            onfocus={() => {
                if (!restoringFocus) inspectCluster(currentCluster);
            }}
            onkeydown={navigate}
            onclick={(event) => {
                pinned = false;
                inspect(event, true);
                void pinDetails();
            }}
        ></canvas>
    </div>
</div>

{#if hovered}
    <!-- svelte-ignore a11y_no_noninteractive_tabindex (Bounded cluster details must be keyboard-scrollable.) -->
    <div
        class="allocation-tooltip"
        id={detailsId}
        role={pinned ? 'region' : 'tooltip'}
        aria-label="Partition cluster details"
        tabindex="0"
        use:mountDetails={hovered}
        onfocus={() => (pinned = true)}
        class:pinned
    >
        {#if pinned}<button
                class="icon-button detail-close"
                aria-label="Close cluster details"
                onclick={() => closeDetails(true)}><Icon name="close" size={14} /></button
            >{/if}
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
        background: var(--color-bg);
        border: 1px solid var(--color-border);
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
    canvas:focus-visible {
        outline: 2px solid var(--state-focus);
        outline-offset: -2px;
    }
    .allocation-tooltip {
        position: fixed;
        z-index: 100;
        display: grid;
        width: min(350px, calc(100vw - 24px));
        gap: 3px;
        padding: 10px 12px;
        color: var(--color-text);
        background: var(--color-panel-raised);
        border: 1px solid var(--color-control-border);
        box-shadow: 0 8px 24px rgb(0 0 0 / 45%);
        box-sizing: border-box;
        overflow: auto;
        overscroll-behavior: contain;
        font-size: 11px;
        pointer-events: none;
    }
    .allocation-tooltip.pinned {
        pointer-events: auto;
    }
    .detail-close {
        position: absolute;
        right: 4px;
        top: 4px;
    }
    .allocation-tooltip > span,
    .owner span {
        color: var(--color-text);
    }
    .owner {
        display: grid;
        gap: 2px;
        margin-top: 6px;
        padding-top: 6px;
        border-top: 1px solid var(--color-border);
    }
    .problem {
        color: var(--color-danger) !important;
    }
</style>
