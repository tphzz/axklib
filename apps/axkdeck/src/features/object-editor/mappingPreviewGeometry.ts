import { MappingGeometryCache, mappingOverlaps, mappingTones, type MappingGeometry } from './keyboardGeometry';
import type { KeyboardMappingPreview, KeyboardRange } from './keyboardMapping';

function intersection(a: KeyboardRange, b: KeyboardRange): KeyboardRange | null {
    const range = {
        low: Math.max(a.low, b.low),
        high: Math.min(a.high, b.high),
        velocityLow: Math.max(a.velocityLow, b.velocityLow),
        velocityHigh: Math.min(a.velocityHigh, b.velocityHigh),
    };
    return range.low <= range.high && range.velocityLow <= range.velocityHigh ? range : null;
}
const path = (range: KeyboardRange) =>
    `M${range.low} ${127 - range.velocityHigh}h${range.high - range.low + 1}v${range.velocityHigh - range.velocityLow + 1}h-${range.high - range.low + 1}Z`;

export class MappingPreviewGeometryCache {
    private original?: MappingGeometry;
    private ids?: ReadonlySet<string>;
    private background?: MappingGeometry;
    private colors?: Map<string, string>;
    private readonly backgroundCache = new MappingGeometryCache();

    read(
        geometry: MappingGeometry,
        preview?: KeyboardMappingPreview,
    ): { geometry: MappingGeometry; overlapPath?: string } {
        if (!preview) return { geometry };
        if (this.original !== geometry || this.ids !== preview.ids) {
            this.original = geometry;
            this.ids = preview.ids;
            this.background = this.backgroundCache.read(geometry.zones.filter((zone) => !preview.ids.has(zone.id)));
            this.colors = new Map(geometry.colors);
            geometry.zones.forEach((zone, index) => {
                if (!this.colors!.has(zone.id)) this.colors!.set(zone.id, mappingTones[index % 2]!);
            });
        }
        const background = this.background!;
        const changed = new Map(preview.zones.map((zone) => [zone.id, zone]));
        const active = preview.zones.filter((zone) => !zone.empty);
        const zones = geometry.zones.map((zone) => changed.get(zone.id) ?? zone);
        const orderedIds = new Set(geometry.ordered.map((zone) => zone.id));
        const coverage = background.coverage.map((members, note) => {
            const added = active.filter((zone) => note >= zone.low && note <= zone.high);
            return added.length ? [...members, ...added] : members;
        });
        // Unchanged overlaps are cached. Only moving regions need new intersections.
        const areas: KeyboardRange[] = [...background.overlaps, ...mappingOverlaps(active)];
        for (const zone of active)
            for (const other of background.zones) {
                if (other.empty) continue;
                const overlap = intersection(zone, other);
                if (overlap) areas.push(overlap);
            }
        return {
            geometry: {
                ...geometry,
                zones,
                coverage,
                ordered: [
                    ...geometry.ordered.map((zone) => changed.get(zone.id) ?? zone),
                    ...active.filter((zone) => !orderedIds.has(zone.id)),
                ],
                colors: this.colors!,
            },
            overlapPath: [...new Set(areas.map(path))].join(''),
        };
    }
}
