import { describe, expect, it } from 'vitest';
import {
    allocationConsistencyLabel,
    allocationGridCluster,
    allocationGridColumns,
    allocationRunVisualKind,
    findAllocationRun,
    type AllocationRun,
} from './allocationGrid';

const runs = [
    { startCluster: 0, clusterCount: 4 },
    { startCluster: 4, clusterCount: 7 },
] as AllocationRun[];

describe('allocation grid indexing', () => {
    it('finds the run containing either boundary cluster', () => {
        expect(findAllocationRun(runs, 0)).toBe(runs[0]);
        expect(findAllocationRun(runs, 3)).toBe(runs[0]);
        expect(findAllocationRun(runs, 4)).toBe(runs[1]);
        expect(findAllocationRun(runs, 10)).toBe(runs[1]);
        expect(findAllocationRun(runs, 11)).toBeNull();
    });

    it('maps visible canvas cells back to absolute clusters', () => {
        expect(allocationGridColumns(81, 7)).toBe(10);
        expect(allocationGridCluster(0, 0, 10, 12, 7)).toBe(120);
        expect(allocationGridCluster(8, 8, 10, 12, 7)).toBe(131);
        expect(allocationGridCluster(7, 0, 10, 12, 7)).toBeNull();
    });
});

describe('allocation grid diagnostics', () => {
    function run(allocationKind: AllocationRun['allocationKind'], consistencyFlags: AllocationRun['consistencyFlags']) {
        return { allocationKind, consistencyFlags } as AllocationRun;
    }

    it('keeps reserved metadata distinct from allocation faults', () => {
        expect(allocationRunVisualKind(run('RESERVED', []))).toBe('reserved');
    });

    it('distinguishes filesystem records from sampler object data', () => {
        expect(allocationRunVisualKind(run('DIRECTORY', []))).toBe('directory');
        expect(allocationRunVisualKind(run('SUPPORT', []))).toBe('support');
        expect(allocationRunVisualKind(run('DATA', []))).toBe('data');
        expect(allocationRunVisualKind(run('UNKNOWN', []))).toBe('unknown');
    });

    it('distinguishes missing bitmap allocation from multiple ownership', () => {
        expect(allocationRunVisualKind(run('DATA', ['CLAIMED_BUT_FREE']))).toBe('claimed-but-free');
        expect(allocationRunVisualKind(run('CONFLICT', ['MULTIPLE_CLAIMS']))).toBe('multiple-claims');
        expect(allocationConsistencyLabel('CLAIMED_BUT_FREE')).toBe('Index claim, bitmap free');
        expect(allocationConsistencyLabel('MULTIPLE_CLAIMS')).toBe('Multiple allocation claims');
    });
});
