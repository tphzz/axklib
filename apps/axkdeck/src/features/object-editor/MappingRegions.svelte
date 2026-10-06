<script lang="ts">
    import { mappingFill, mappingRectangle, mappingTones, type MappingGeometry } from './keyboardGeometry';
    import { mappingLabels, type VelocityTick } from './mappingPresentation';
    import type { KeyboardRange, KeyboardZone } from './keyboardMapping';

    let {
        zones,
        geometry,
        start,
        span,
        width,
        height,
        grid,
        formatNote,
        onselect,
        ondrag,
    }: {
        zones: KeyboardZone[];
        geometry: MappingGeometry;
        start: number;
        span: number;
        width: number;
        height: number;
        grid: { major: VelocityTick[]; minor: VelocityTick[] };
        formatNote: (note: number) => string;
        onselect: (id: string) => void;
        ondrag?: (event: PointerEvent, ids: string[]) => void;
    } = $props();
    const colors = $derived(geometry.colors);
    const overlaps = $derived(geometry.overlaps);
    const borders = $derived(
        mappingTones.map((tone) => ({
            tone,
            path: [
                ...new Set(
                    geometry.zones
                        .filter((zone) => !zone.empty && colors.get(zone.id) === tone)
                        .map(
                            (zone) =>
                                `M${zone.low} ${127 - zone.velocityHigh}h${zone.high - zone.low + 1}v${zone.velocityHigh - zone.velocityLow + 1}h-${zone.high - zone.low + 1}Z`,
                        ),
                ),
            ].join(''),
        })),
    );
    const selected = $derived(zones.filter((zone) => zone.selected && !zone.empty));
    const selectedIds = $derived(new Set(selected.map((zone) => zone.id)));
    const bounds = $derived<KeyboardRange | null>(
        selected.length
            ? {
                  low: Math.min(...selected.map((zone) => zone.low)),
                  high: Math.max(...selected.map((zone) => zone.high)),
                  velocityLow: Math.min(...selected.map((zone) => zone.velocityLow)),
                  velocityHigh: Math.max(...selected.map((zone) => zone.velocityHigh)),
              }
            : null,
    );
    const labels = $derived(mappingLabels(zones, start, span, width, height, geometry.ordered));
    const keyLines = $derived({
        minor: Array.from({ length: 129 }, (_, note) => note)
            .filter((note) => note % 12 !== 0)
            .map((note) => `M${note} 0V128`)
            .join(''),
        major: Array.from({ length: 11 }, (_, index) => `M${index * 12} 0V128`).join(''),
    });
    const velocityLines = $derived({
        minor: grid.minor.map((tick) => `M${start} ${127 - tick.value + 0.5}H${start + span}`).join(''),
        major: grid.major.map((tick) => `M${start} ${127 - tick.value + 0.5}H${start + span}`).join(''),
    });
</script>

<div class="region-fills" style:transform={`translate3d(${(-start / span) * 100}%, 0, 0) scaleX(${128 / span})`}>
    {#each geometry.zones as zone (zone.id)}
        {#if !zone.empty}
            {@const box = mappingRectangle(zone, 0, 128)}
            <button
                class="zone"
                class:chosen={selectedIds.has(zone.id)}
                style:--zone-color={colors.get(zone.id)}
                style:background={mappingFill(colors.get(zone.id), selectedIds.has(zone.id))}
                title={`${zone.label}: ${formatNote(zone.low)}–${formatNote(zone.high)}, velocity ${zone.velocityLow}–${zone.velocityHigh}`}
                aria-label={`Select mapping ${zone.label}`}
                aria-pressed={selectedIds.has(zone.id)}
                style:left={`${box.left}%`}
                style:width={`${box.width}%`}
                style:top={`${box.top}%`}
                style:height={`${box.height}%`}
                onclick={() => onselect(zone.id)}
                onpointerdown={(event) => ondrag?.(event, [zone.id])}
            ></button>
        {/if}
    {/each}
    {#each overlaps as overlap}
        {@const box = mappingRectangle(overlap, 0, 128)}
        <button
            class="overlap"
            title={overlap.zones.map((zone) => zone.label).join(', ')}
            aria-label={`Select overlapping mappings: ${overlap.zones.map((zone) => zone.label).join(', ')}`}
            style:left={`${box.left}%`}
            style:top={`${box.top}%`}
            style:width={`${box.width}%`}
            style:height={`${box.height}%`}
            onpointerdown={(event) =>
                ondrag?.(
                    event,
                    overlap.zones.map((zone) => zone.id),
                )}
            onclick={() =>
                onselect(
                    overlap.zones[
                        (overlap.zones.findIndex((zone) => selectedIds.has(zone.id)) + 1) % overlap.zones.length
                    ]!.id,
                )}
        ></button>
    {/each}
</div>
<svg class="region-borders" viewBox={`${start} 0 ${span} 128`} preserveAspectRatio="none" aria-hidden="true">
    {#each borders as border}
        <path class="zone-border" style:--zone-color={border.tone} d={border.path} />
    {/each}
</svg>
<svg class="grid-overlay" viewBox={`${start} 0 ${span} 128`} preserveAspectRatio="none" aria-hidden="true">
    <path class="grid" d={keyLines.minor} />
    <path class="grid major" d={keyLines.major} />
    <path class="grid velocity-minor" d={velocityLines.minor} />
    <path class="grid velocity-major" d={velocityLines.major} />
    {#each geometry.sources as source}
        <rect
            class="source"
            x={source.low}
            width={source.high - source.low + 1}
            y={127 - source.velocityHigh}
            height={source.velocityHigh - source.velocityLow + 1}
        />
    {/each}
</svg>
{#each selected as zone (zone.id)}
    {@const box = mappingRectangle(zone, start, span)}
    <div
        class="selection-outline"
        style:--zone-color={colors.get(zone.id)}
        style:background={mappingFill(colors.get(zone.id), true)}
        style:left={`${box.left}%`}
        style:width={`${box.width}%`}
        style:top={`${box.top}%`}
        style:height={`${box.height}%`}
    ></div>
{/each}
<svg class="guides" viewBox={`${start} 0 ${span} 128`} preserveAspectRatio="none" aria-hidden="true">
    {#if bounds}
        <line class="range-guide" data-boundary="low" x1={bounds.low} x2={bounds.low} y1="0" y2="128" />
        <line class="range-guide" data-boundary="high" x1={bounds.high + 1} x2={bounds.high + 1} y1="0" y2="128" />
        <line
            class="range-guide"
            data-boundary="velocityHigh"
            x1={start}
            x2={start + span}
            y1={127 - bounds.velocityHigh}
            y2={127 - bounds.velocityHigh}
        />
        <line
            class="range-guide"
            data-boundary="velocityLow"
            x1={start}
            x2={start + span}
            y1={128 - bounds.velocityLow}
            y2={128 - bounds.velocityLow}
        />
    {/if}
</svg>
{#each labels as label (label.id)}
    <span
        class="mapping-label"
        class:chosen={label.selected}
        data-zone={label.id}
        style:left={`${label.left}px`}
        style:top={`${label.top}px`}
        style:height={`${label.height}px`}
        aria-hidden="true">{label.label}</span
    >
{/each}

<style>
    .region-fills {
        position: absolute;
        inset: 0;
        transform-origin: 0 0;
    }
    .zone,
    .overlap,
    .selection-outline {
        position: absolute;
        padding: 0;
        border: 0;
        border-radius: 0;
    }
    .zone {
        touch-action: none;
        cursor: move;
    }
    .overlap {
        touch-action: none;
        cursor: move;
        background: repeating-linear-gradient(
            135deg,
            transparent 0 6px,
            color-mix(in srgb, var(--editor-loop) 10%, transparent) 6px 8px
        );
    }
    .selection-outline {
        box-shadow: inset 0 0 0 2px var(--color-text);
        pointer-events: none;
    }
    svg {
        position: absolute;
        inset: 0;
        width: 100%;
        height: 100%;
        pointer-events: none;
    }
    .zone-border {
        fill: none;
        stroke: color-mix(in srgb, var(--zone-color) 65%, transparent);
        stroke-width: 1;
        vector-effect: non-scaling-stroke;
    }
    .grid {
        stroke: var(--color-border);
        stroke-width: 0.5;
        vector-effect: non-scaling-stroke;
        fill: none;
        opacity: 0.4;
    }
    .grid.major {
        stroke: var(--color-text-muted);
        opacity: 0.35;
    }
    .grid.velocity-major {
        stroke: var(--color-text-muted);
        stroke-width: 1;
        opacity: 0.45;
    }
    .source {
        fill: none;
        stroke: var(--color-text-muted);
        stroke-dasharray: 3 3;
        vector-effect: non-scaling-stroke;
    }
    .range-guide {
        stroke: var(--color-text);
        stroke-width: 1;
        stroke-dasharray: 4 3;
        vector-effect: non-scaling-stroke;
        opacity: 0.8;
    }
    .mapping-label {
        position: absolute;
        width: 12px;
        overflow: hidden;
        white-space: nowrap;
        text-overflow: ellipsis;
        writing-mode: vertical-rl;
        color: var(--color-text-muted);
        font: 10px/12px var(--font-sans);
        pointer-events: none;
    }
    .mapping-label.chosen {
        color: var(--color-text);
    }
    button:focus-visible {
        outline: 2px solid var(--color-text);
        outline-offset: -2px;
    }
</style>
