import type { PlotPoint } from './ParameterGraph.svelte';

export function tracePosition(points: PlotPoint[], x: number): PlotPoint | undefined {
    if (!points.length || x < 0 || x > 1) return;
    const next = points.findIndex((point) => point.x >= x);
    if (next < 0) return points.at(-1);
    const right = points[next]!,
        left = points[Math.max(0, next - 1)]!;
    return { x, y: right.x === left.x ? right.y : left.y + ((right.y - left.y) * (x - left.x)) / (right.x - left.x) };
}
