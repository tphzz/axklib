import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({ invoke: vi.fn() }));

vi.mock('@tauri-apps/api/core', () => ({ invoke: mocks.invoke }));

import {
    allocationExportFilename,
    allocationSpaceStatistic,
    formatAllocationBytes,
    saveAllocationMap,
} from './allocationInspector';

describe('allocation inspector presentation', () => {
    beforeEach(() => mocks.invoke.mockReset());

    it('presents free clusters as allocatable bytes with the cluster count retained', () => {
        expect(allocationSpaceStatistic(10, 1024)).toEqual({ primary: '10 KiB', secondary: '10 clusters' });
        expect(allocationSpaceStatistic(1, 1024)).toEqual({ primary: '1 KiB', secondary: '1 cluster' });
        expect(formatAllocationBytes(503.2 * 1024)).toBe('503.2 KiB');
    });

    it('creates a filesystem-safe allocation export filename', () => {
        expect(allocationExportFilename('Sounds & Tests', 2)).toBe('Sounds_Tests-allocation-map.json');
        expect(allocationExportFilename('', 2)).toBe('partition-3-allocation-map.json');
    });

    it('delegates JSON saving to the native desktop command', async () => {
        const document = { partitionName: 'Sounds', clusterSizeBytes: 1024 };
        mocks.invoke.mockResolvedValue('/tmp/Sounds-allocation-map.json');

        await expect(saveAllocationMap('Sounds-allocation-map.json', document)).resolves.toBe(
            '/tmp/Sounds-allocation-map.json',
        );
        expect(mocks.invoke).toHaveBeenCalledWith('save_allocation_map_json', {
            request: { suggestedName: 'Sounds-allocation-map.json', document },
        });
    });

    it('treats a cancelled native save dialog as a normal result', async () => {
        mocks.invoke.mockResolvedValue(null);
        await expect(saveAllocationMap('partition-1-allocation-map.json', {})).resolves.toBeNull();
    });
});
