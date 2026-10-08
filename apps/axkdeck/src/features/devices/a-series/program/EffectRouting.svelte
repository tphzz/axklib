<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import { programRoutes } from './routing';
    import { measureWidth } from '../../../object-editor/measureWidth';
    import ExtendedParameterMarker from '../../../object-editor/ExtendedParameterMarker.svelte';
    let {
        document,
        selected,
        onselect,
    }: { document: ObjectEditorDocument; selected: number; onselect: (slot: number) => void } = $props();
    const format = $derived(document.programFormat!);
    const count = $derived(format.model === 'A3000' ? 3 : 6);
    const routes = $derived(programRoutes(document.draft.values, count));
    const id = $props.id();
    let measured = $state(420);
    const width = $derived(Math.max(400, measured));
    const nodeWidth = $derived(Math.min(150, (width - 80) / 3));
    const x = (slot: number) => ((slot - 1) % 3) * (width / 3) + (width / 3 - nodeWidth) / 2;
    const y = (slot: number) => (slot <= 3 ? 24 : 154);
    function path(from: number, to: number): string {
        if (from <= 3 && to > 3)
            return `M ${x(from) + nodeWidth / 2} ${y(from) + 44} V ${y(from) + 94 + (from - 1) * 8} H ${x(to) + nodeWidth / 2} V ${y(to) - 5}`;
        return from < to
            ? `M ${x(from) + nodeWidth} ${y(from) + 22} H ${x(to) - 5}`
            : `M ${x(from)} ${y(from) + 22} H ${x(to) + nodeWidth + 5}`;
    }
</script>

<div class="routing-scroll" use:measureWidth={{ scope: 'routing', change: (value) => (measured = value) }}>
    <div
        class="routing"
        style:width={`${width}px`}
        style:--node-width={`${nodeWidth}px`}
        style:height={count === 3 ? '120px' : '250px'}
        aria-label="Effect signal routing"
    >
        <svg {width} height={count === 3 ? 120 : 250} aria-hidden="true">
            <defs
                ><marker id={`${id}-arrow`} markerWidth="6" markerHeight="6" refX="5" refY="3" orient="auto"
                    ><path d="M0 0 L6 3 L0 6" fill="var(--editor-loop)" /></marker
                ></defs
            >
            {#each routes as route}
                {#if typeof route.to === 'number'}
                    <path class="route" d={path(route.from, route.to)} marker-end={`url(#${id}-arrow)`} />
                {:else}
                    <path
                        class="route"
                        d={`M ${x(route.from) + nodeWidth / 2} ${y(route.from) + 44} V ${y(route.from) + 62}`}
                        marker-end={`url(#${id}-arrow)`}
                    />
                {/if}
            {/each}
        </svg>
        {#each Array.from({ length: count }, (_, i) => i + 1) as slot}
            {@const type = format.effects.find((effect) => effect.id === document.draft.values[`effects.${slot}.type`])}
            {@const route = routes.find((route) => route.from === slot)}
            <span class="input" style:left={`${x(slot)}px`} style:top={`${y(slot) - 18}px`}
                >Input Ef{slot}{#if slot > 3}<ExtendedParameterMarker a5000Only />{/if}</span
            >
            <button
                class:selected={selected === slot}
                class:bypassed={document.draft.values[`effects.${slot}.enabled`] === false}
                style:left={`${x(slot)}px`}
                style:top={`${y(slot)}px`}
                aria-label={`Select Ef${slot}`}
                title={`Ef${slot}: ${type?.label ?? 'Unknown effect'}${slot > 3 ? ' (A5000 only)' : ''}`}
                aria-pressed={selected === slot}
                onclick={() => onselect(slot)}
            >
                <strong>Ef{slot}{document.draft.values[`effects.${slot}.enabled`] === false ? ' · Bypass' : ''}</strong>
                <span>{type?.label ?? 'Unknown effect'}</span>
            </button>
            {#if route && typeof route.to === 'string'}<span
                    class="output"
                    style:left={`${x(slot)}px`}
                    style:top={`${y(slot) + 70}px`}>{route.to}</span
                >{/if}
        {/each}
    </div>
</div>

<style>
    .routing-scroll {
        overflow-x: auto;
        flex-shrink: 0;
        background: var(--color-panel-deep);
    }
    .routing {
        position: relative;
        margin: auto;
    }
    svg {
        position: absolute;
        inset: 0;
        pointer-events: none;
    }
    .route {
        fill: none;
        stroke: var(--editor-loop);
        stroke-width: 1.5;
    }
    button {
        position: absolute;
        width: var(--node-width);
        height: 44px;
        display: flex;
        flex-direction: column;
        justify-content: center;
        gap: 4px;
        padding: 5px 8px;
        border: 1px solid var(--color-border);
        border-radius: 3px;
        background: var(--color-panel);
        font-size: 11px;
        line-height: 13px;
        text-align: left;
    }
    button span {
        flex: 0 0 13px;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        max-width: 100%;
    }
    button.selected {
        border-color: var(--color-accent);
        background: var(--color-panel-raised);
    }
    button.bypassed {
        border-style: dashed;
        color: var(--color-text-muted);
    }
    .input,
    .output {
        position: absolute;
        width: var(--node-width);
        text-align: center;
        font-size: 10px;
        color: var(--color-text-muted);
    }
</style>
