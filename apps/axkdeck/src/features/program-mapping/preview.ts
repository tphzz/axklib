import type { KeyboardMappingPreview, KeyboardRange } from '../object-editor/keyboardMapping';
import type { MappingSnapshot } from './protocol';
import { validRange } from './protocol';

export function createMappingPreview(
    snapshot: Pick<MappingSnapshot, 'role' | 'limits' | 'zones' | 'selectionId' | 'overrides'>,
) {
    if (!snapshot.limits) return null;
    const original = snapshot.limits;
    const zones = snapshot.zones.filter(
        (zone) => snapshot.role === 'bank' || zone.selectionId === snapshot.selectionId,
    );
    const ids = new Set(zones.map((zone) => zone.id));
    const overridden = new Set(snapshot.overrides.filter((row) => !row.inherited).map((row) => row.boundary));
    return (range: KeyboardRange): KeyboardMappingPreview => ({
        ids,
        zones: zones.map((zone) => {
            let projected = range;
            if (snapshot.role === 'program') {
                const source = zone.source ?? zone;
                projected = {
                    low: Math.max(source.low, range.low),
                    high: Math.min(source.high, range.high),
                    velocityLow: Math.max(source.velocityLow, range.velocityLow),
                    velocityHigh: Math.min(source.velocityHigh, range.velocityHigh),
                };
            } else if (snapshot.role === 'bank') {
                const source = zone.source ?? zone;
                projected = {
                    ...source,
                    velocityLow:
                        overridden.has('velocityLow') || range.velocityLow !== original.velocityLow
                            ? range.velocityLow
                            : source.velocityLow,
                    velocityHigh:
                        overridden.has('velocityHigh') || range.velocityHigh !== original.velocityHigh
                            ? range.velocityHigh
                            : source.velocityHigh,
                };
            }
            return { ...zone, ...projected, empty: !validRange(projected) };
        }),
    });
}
