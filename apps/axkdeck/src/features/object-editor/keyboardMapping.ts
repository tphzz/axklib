export interface KeyboardRange {
    low: number;
    high: number;
    velocityLow: number;
    velocityHigh: number;
}
export interface KeyboardZone extends KeyboardRange {
    id: string;
    label: string;
    root?: number;
    selected?: boolean;
    source?: KeyboardRange;
    empty?: boolean;
}
export type RangeHandle = 'move' | 'low' | 'high' | 'velocityLow' | 'velocityHigh';
const clamp = (value: number, low: number, high: number) => Math.max(low, Math.min(high, Math.round(value)));
export function editKeyboardRange(
    range: KeyboardRange,
    handle: RangeHandle,
    notes: number,
    velocity: number,
): KeyboardRange {
    const result = { ...range };
    if (handle === 'move') {
        const dx = clamp(notes, -range.low, 127 - range.high);
        const dy = clamp(velocity, -range.velocityLow, 127 - range.velocityHigh);
        result.low += dx;
        result.high += dx;
        result.velocityLow += dy;
        result.velocityHigh += dy;
    } else if (handle === 'low') result.low = clamp(range.low + notes, 0, range.high);
    else if (handle === 'high') result.high = clamp(range.high + notes, range.low, 127);
    else if (handle === 'velocityLow') result.velocityLow = clamp(range.velocityLow + velocity, 0, range.velocityHigh);
    else result.velocityHigh = clamp(range.velocityHigh + velocity, range.velocityLow, 127);
    return result;
}
