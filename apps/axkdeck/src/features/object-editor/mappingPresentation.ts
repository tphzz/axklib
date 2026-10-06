import { mappingRectangle, orderedMappings } from './keyboardGeometry';
import type { KeyboardZone } from './keyboardMapping';

export interface VelocityTick {
    value: number;
    position: number;
}

function createVelocityGrid(): { major: VelocityTick[]; minor: VelocityTick[] } {
    const tick = (value: number) => ({ value, position: ((127 - value + 0.5) / 128) * 100 });
    const values = Array.from({ length: 26 }, (_, index) => index * 5);
    return {
        major: values.filter((value) => value % 25 === 0).map(tick),
        minor: values.filter((value) => value % 25 !== 0).map(tick),
    };
}
const grid = createVelocityGrid();
export function velocityGrid() {
    return grid;
}

export interface MappingLabel {
    id: string;
    label: string;
    selected: boolean;
    left: number;
    top: number;
    height: number;
}

export function mappingLabels(
    zones: KeyboardZone[],
    start: number,
    span: number,
    width: number,
    height: number,
    geometryOrder = orderedMappings(zones),
): MappingLabel[] {
    const labels: MappingLabel[] = [];
    const selected = new Set(zones.filter((zone) => zone.selected).map((zone) => zone.id));
    const ordered = [
        ...geometryOrder.filter((zone) => selected.has(zone.id)),
        ...geometryOrder.filter((zone) => !selected.has(zone.id)),
    ];
    for (const zone of ordered) {
        const box = mappingRectangle(zone, start, span);
        const left = (Math.max(0, box.left) * width) / 100;
        const right = (Math.min(100, box.left + box.width) * width) / 100;
        const top = (Math.max(0, box.top) * height) / 100;
        const bottom = (Math.min(100, box.top + box.height) * height) / 100;
        if (right - left < 18 || bottom - top < 32) continue;
        const label = {
            id: zone.id,
            label: zone.label,
            selected: selected.has(zone.id),
            left: left + 3,
            top: top + 4,
            height: bottom - top - 8,
        };
        if (
            labels.some(
                (other) =>
                    label.left < other.left + 12 &&
                    label.left + 12 > other.left &&
                    label.top < other.top + other.height &&
                    label.top + label.height > other.top,
            )
        )
            continue;
        labels.push(label);
    }
    return labels;
}
