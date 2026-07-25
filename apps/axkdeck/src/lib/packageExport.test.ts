import { describe, expect, it } from 'vitest';
import { packageExportFilename } from './packageExport';
import type { PackageExportObjectKind, PackageExportSelection } from './types';

const object = (kind: PackageExportObjectKind, name: string): PackageExportSelection => ({
    kind,
    objectId: `${kind}-${name}`,
    name,
    typeLabel:
        kind === 'PROGRAM' ? 'Program' : kind === 'SBAC' ? 'Sample Bank' : kind === 'SBNK' ? 'Sample' : 'Wave Data',
    partitionIndex: 0,
    partitionName: 'Partition 0',
    volumeName: 'Volume',
});

const volume = (name: string): PackageExportSelection => ({
    kind: 'VOLUME',
    partitionIndex: 0,
    volumeName: name,
    name,
    typeLabel: 'Volume',
});

describe('package export filenames', () => {
    it.each([
        [[volume('Volume A'), volume('Volume B')], 'Volume A and others.axkvol'],
        [[object('PROGRAM', 'Program A'), object('PROGRAM', 'Program B')], 'Program A and others.axkprg'],
        [[object('SBAC', 'Bank A'), object('SBAC', 'Bank B')], 'Bank A and others.axksbac'],
        [[object('SBNK', 'Sample A'), object('SBNK', 'Sample B')], 'Sample A and others.axksbnk'],
        [[object('SMPL', 'Wave A'), object('SMPL', 'Wave B')], 'Wave A and others.axksmpl'],
    ])('uses the typed extension for homogeneous roots', (items, expected) => {
        expect(packageExportFilename(items)).toBe(expected);
    });

    it('uses the generic extension only for mixed root kinds', () => {
        expect(packageExportFilename([object('SBAC', 'Bank'), object('SBNK', 'Sample')])).toBe(
            'Bank and others.axkpkg',
        );
    });
});
