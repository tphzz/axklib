export const graphLayout = $state({ ratio: 0.5, views: {} as Record<string, number> });

export function graphSplitRatio(ratio: number, available: number, graphMinimum = 360, controlsMinimum = 360): number {
    const space = Math.max(1, available);
    const minimum = Math.min(graphMinimum / (graphMinimum + controlsMinimum), graphMinimum / space);
    const maximum = Math.max(minimum, 1 - controlsMinimum / space);
    return Math.max(minimum, Math.min(maximum, ratio));
}
