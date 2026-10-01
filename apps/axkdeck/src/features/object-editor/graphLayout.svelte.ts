export const graphLayout = $state({ ratio: 0.5 });

export function graphSplitRatio(ratio: number, available: number): number {
    const minimum = Math.min(0.5, 360 / Math.max(1, available));
    return Math.max(minimum, Math.min(1 - minimum, ratio));
}
