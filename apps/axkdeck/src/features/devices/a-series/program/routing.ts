import type { EditorValues } from '../../../object-editor/draft.svelte';

export const effectDestinations = [
    'Stereo Out',
    'Assign L&R',
    'Assign 1&2',
    'Assign 3&4',
    'Assign 5&6',
    'DIG & OPT',
    'Ef4',
    'Ef5',
    'Ef6',
];
export const connectionLabels = ['Parallel', '1 > 2 / 3', '1 > 2 > 3', '1 / 2 > 3', '1 > 2 < 3'];
const connections: [number, number][][] = [
    [],
    [[0, 1]],
    [
        [0, 1],
        [1, 2],
    ],
    [[1, 2]],
    [
        [0, 1],
        [2, 1],
    ],
];
export function connectionEdges(choice: number, first: number): [number, number][] {
    return (connections[choice] ?? []).map(([from, to]) => [from + first, to + first]);
}
export interface EffectRoute {
    from: number;
    to: number | string;
    kind: 'connection' | 'destination';
}
export function programRoutes(values: EditorValues, count: number): EffectRoute[] {
    const result: EffectRoute[] = [];
    for (let first = 1; first <= count; first += 3) {
        const choice = Number(values[`effect_connections.${first === 1 ? 1 : 2}`]);
        for (const [from, to] of connectionEdges(choice, first)) result.push({ from, to, kind: 'connection' });
    }
    for (let slot = 1; slot <= count; slot++) {
        if (result.some((edge) => edge.from === slot)) continue;
        const destination = Number(values[`effects.${slot}.destination`]);
        if (!Number.isInteger(destination) || destination < 0 || destination > (slot <= 3 && count === 6 ? 8 : 5))
            continue;
        result.push({
            from: slot,
            to: destination < 6 ? effectDestinations[destination]! : destination - 2,
            kind: 'destination',
        });
    }
    return result;
}
