import type { KeyboardRange, KeyboardZone } from './keyboardMapping';

export const isBlackKey = (note: number) => [1, 3, 6, 8, 10].includes(((note % 12) + 12) % 12);

export function keyFace(note: number) {
    if (isBlackKey(note)) return { x: note + 0.08, width: 0.84, height: 18, center: note + 0.5 };
    const x = note - (note > 0 && isBlackKey(note - 1) ? 0.5 : 0);
    const width = 1 + (note > 0 && isBlackKey(note - 1) ? 0.5 : 0) + (note < 127 && isBlackKey(note + 1) ? 0.5 : 0);
    return { x, width, height: 28, center: x + width / 2 };
}

export function mappingRectangle(range: KeyboardRange, start: number, span: number) {
    const left = ((range.low - start) / span) * 100;
    const width = ((range.high - range.low + 1) / span) * 100;
    const top = ((127 - range.velocityHigh) / 128) * 100;
    const height = ((range.velocityHigh - range.velocityLow + 1) / 128) * 100;
    return { left, width, top, height, centerX: left + width / 2, centerY: top + height / 2 };
}

export const mappingTones = ['var(--editor-loop)', 'color-mix(in srgb, var(--editor-loop) 90%, black)'] as const;

export function mappingFill(tone: string = mappingTones[0], selected = false): string {
    return `color-mix(in srgb, ${tone} ${selected ? 50 : 25}%, var(--color-panel-deep))`;
}

export function orderedMappings(zones: KeyboardZone[]) {
    return zones
        .filter((zone) => !zone.empty)
        .sort((a, b) => {
            const left = a.source ?? a,
                right = b.source ?? b;
            return (
                left.low - right.low ||
                left.high - right.high ||
                left.velocityLow - right.velocityLow ||
                left.velocityHigh - right.velocityHigh ||
                (a.id < b.id ? -1 : a.id > b.id ? 1 : 0)
            );
        });
}

export function mappingColors(zones: KeyboardZone[]) {
    return new Map(orderedMappings(zones).map((zone, index) => [zone.id, mappingTones[index % 2]!]));
}

export function keyCoverage(zones: KeyboardZone[], note: number) {
    return zones.filter((zone) => !zone.empty && note >= zone.low && note <= zone.high);
}

export interface MappingOverlap extends KeyboardRange {
    zones: KeyboardZone[];
}
function coverageIndex(zones: KeyboardZone[]) {
    const coverage: KeyboardZone[][] = Array.from({ length: 128 }, () => []);
    const edges = [
        ...new Set([0, 128, ...zones.filter((zone) => !zone.empty).flatMap((zone) => [zone.low, zone.high + 1])]),
    ]
        .filter((edge) => edge >= 0 && edge <= 128)
        .sort((a, b) => a - b);
    for (let index = 0; index < edges.length - 1; index++) {
        const low = edges[index]!,
            high = edges[index + 1]!;
        const active = keyCoverage(zones, low);
        for (let note = low; note < high; note++) coverage[note] = active;
    }
    return coverage;
}
export function mappingOverlaps(zones: KeyboardZone[], coverage = coverageIndex(zones)): MappingOverlap[] {
    const result: MappingOverlap[] = [];
    const previous = new Map<string, MappingOverlap>();
    const order = new Map(zones.map((zone, index) => [zone, index]));
    for (let note = 0; note < 128;) {
        const members = coverage[note]!;
        let high = note;
        while (high < 127 && coverage[high + 1] === members) high++;
        if (members.length < 2) {
            note = high + 1;
            continue;
        }
        const events = new Map<number, { enter: KeyboardZone[]; leave: KeyboardZone[] }>();
        for (const zone of members) {
            for (const [edge, kind] of [
                [zone.velocityLow, 'enter'],
                [zone.velocityHigh + 1, 'leave'],
            ] as const) {
                if (!events.has(edge)) events.set(edge, { enter: [], leave: [] });
                events.get(edge)![kind].push(zone);
            }
        }
        const edges = [...events.keys()].sort((a, b) => a - b);
        const activeSet = new Set<KeyboardZone>();
        for (let index = 0; index < edges.length - 1; index++) {
            const velocityLow = edges[index]!,
                velocityHigh = edges[index + 1]! - 1;
            const event = events.get(velocityLow)!;
            for (const zone of event.leave) activeSet.delete(zone);
            for (const zone of event.enter) activeSet.add(zone);
            const active = members.filter((zone) => activeSet.has(zone));
            if (active.length < 2) continue;
            const key = `${velocityLow}:${velocityHigh}:${active.map((zone) => order.get(zone)).join(',')}`;
            const last = previous.get(key);
            if (last && last.high === note - 1) last.high = high;
            else {
                const overlap = { low: note, high, velocityLow, velocityHigh, zones: active };
                result.push(overlap);
                previous.set(key, overlap);
            }
        }
        note = high + 1;
    }
    return result;
}

export interface MappingGeometry {
    zones: KeyboardZone[];
    coverage: KeyboardZone[][];
    overlaps: MappingOverlap[];
    colors: Map<string, string>;
    ordered: KeyboardZone[];
    sources: KeyboardRange[];
}
const rangeKeys = ['low', 'high', 'velocityLow', 'velocityHigh'] as const;
const geometryKeys = ['id', 'label', ...rangeKeys, 'root', 'empty'] as const;
export class MappingGeometryCache {
    private cached: MappingGeometry | undefined;
    read(zones: KeyboardZone[]): MappingGeometry {
        const previous = this.cached?.zones;
        if (
            previous &&
            previous.length === zones.length &&
            zones.every((zone, index) => {
                const old = previous[index]!;
                return (
                    geometryKeys.every((key) => Object.is(zone[key], old[key])) &&
                    !!zone.source === !!old.source &&
                    rangeKeys.every((key) => Object.is(zone.source?.[key], old.source?.[key]))
                );
            })
        )
            return this.cached!;
        // Own immutable geometry copies; caller snapshots and selection can change independently.
        const captured = zones.map((zone) => ({
            ...zone,
            selected: false,
            source: zone.source ? { ...zone.source } : undefined,
        }));
        const coverage = coverageIndex(captured);
        const ordered = orderedMappings(captured);
        const sources = new Map<string, KeyboardRange>();
        for (const zone of captured)
            if (
                zone.source &&
                zone.source.low <= zone.source.high &&
                zone.source.velocityLow <= zone.source.velocityHigh
            ) {
                const source = zone.source;
                sources.set(rangeKeys.map((key) => source[key]).join(':'), source);
            }
        this.cached = {
            zones: captured,
            coverage,
            ordered,
            sources: [...sources.values()],
            colors: new Map(ordered.map((zone, index) => [zone.id, mappingTones[index % 2]!])),
            overlaps: mappingOverlaps(captured, coverage),
        };
        return this.cached;
    }
}
