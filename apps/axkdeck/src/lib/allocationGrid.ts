import type { components } from './generated/axklibApiV1';

export type AllocationRun = components['schemas']['ImageAllocationRun'];
export type AllocationConsistencyFlag = AllocationRun['consistencyFlags'][number];
export type AllocationVisualKind =
    | 'multiple-claims'
    | 'claimed-but-free'
    | 'bitmap-mismatch'
    | 'used-without-claim'
    | 'reserved'
    | 'continuation'
    | 'directory'
    | 'support'
    | 'data'
    | 'unknown'
    | 'free';

export const allocationPalette = {
    reserved: { color: '#718096', label: 'Reserved' },
    continuation: { color: '#fbbf24', label: 'Record continuation' },
    directory: { color: '#2dd4bf', label: 'Directory data' },
    support: { color: '#94a3b8', label: 'SFS support data' },
    data: { color: '#34d399', label: 'Object data' },
    unknown: { color: '#f97316', label: 'Unknown record data' },
    free: { color: '#252b2e', label: 'Free' },
    'used-without-claim': { color: '#60a5fa', label: 'Allocated without owner' },
    'bitmap-mismatch': { color: '#e879f9', label: 'Bitmap copies differ' },
    'claimed-but-free': { color: '#fb7185', label: 'Index claim, bitmap free' },
    'multiple-claims': { color: '#ef4444', label: 'Multiple claims' },
} as const satisfies Record<AllocationVisualKind, { color: string; label: string }>;

export const allocationLegendOrder = [
    'reserved',
    'continuation',
    'directory',
    'support',
    'data',
    'unknown',
    'free',
    'used-without-claim',
    'bitmap-mismatch',
    'claimed-but-free',
    'multiple-claims',
] as const satisfies readonly AllocationVisualKind[];

export function allocationRunVisualKind(run: AllocationRun): AllocationVisualKind {
    if (run.consistencyFlags.includes('MULTIPLE_CLAIMS')) return 'multiple-claims';
    if (run.consistencyFlags.includes('CLAIMED_BUT_FREE')) return 'claimed-but-free';
    if (run.consistencyFlags.includes('BITMAP_COPY_MISMATCH')) return 'bitmap-mismatch';
    if (run.consistencyFlags.includes('USED_WITHOUT_CLAIM')) return 'used-without-claim';
    switch (run.allocationKind) {
        case 'RESERVED':
            return 'reserved';
        case 'CONTINUATION':
            return 'continuation';
        case 'DIRECTORY':
            return 'directory';
        case 'SUPPORT':
            return 'support';
        case 'DATA':
            return 'data';
        case 'UNKNOWN':
            return 'unknown';
        case 'UNCLAIMED':
            return 'used-without-claim';
        case 'CONFLICT':
            return 'multiple-claims';
        default:
            return 'free';
    }
}

export function allocationConsistencyLabel(flag: AllocationConsistencyFlag): string {
    switch (flag) {
        case 'BITMAP_COPY_MISMATCH':
            return 'Bitmap copies differ';
        case 'CLAIMED_BUT_FREE':
            return 'Index claim, bitmap free';
        case 'USED_WITHOUT_CLAIM':
            return 'Bitmap allocated, no index claim';
        case 'MULTIPLE_CLAIMS':
            return 'Multiple allocation claims';
    }
}

export function findAllocationRun(runs: readonly AllocationRun[], cluster: number): AllocationRun | null {
    let low = 0;
    let high = runs.length - 1;
    while (low <= high) {
        const middle = Math.floor((low + high) / 2);
        const run = runs[middle];
        if (!run) return null;
        if (cluster < run.startCluster) high = middle - 1;
        else if (cluster >= run.startCluster + run.clusterCount) low = middle + 1;
        else return run;
    }
    return null;
}

export function allocationGridColumns(width: number, cellSize: number, gap = 1): number {
    return Math.max(1, Math.floor((Math.max(1, width) + gap) / (cellSize + gap)));
}

export function allocationGridCluster(
    x: number,
    y: number,
    columns: number,
    firstRow: number,
    cellSize: number,
    gap = 1,
): number | null {
    const pitch = cellSize + gap;
    const column = Math.floor(x / pitch);
    const row = Math.floor(y / pitch);
    if (column < 0 || column >= columns || row < 0) return null;
    if (x % pitch >= cellSize || y % pitch >= cellSize) return null;
    return (firstRow + row) * columns + column;
}
