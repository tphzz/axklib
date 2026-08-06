import { describe, expect, it } from 'vitest';
import { packageExportFilename } from './packageExport';
import { axkObjectDirectoryLocation, serverFileLocation } from './storageLocations';
import type { PackageExportObjectKind, PackageExportSelection } from './types';

const object = (kind: PackageExportObjectKind, name: string): PackageExportSelection => ({
    kind,
    objectId: `${kind}-${name}`,
    name,
    typeLabel:
        kind === 'PROGRAM'
            ? 'Program'
            : kind === 'SEQU'
              ? 'Sequence'
              : kind === 'SBAC'
                ? 'Sample Bank'
                : kind === 'SBNK'
                  ? 'Sample'
                  : 'Wave Data',
    partitionIndex: 0,
    partitionName: 'Partition 0',
    volumeName: 'Volume',
});

const volume = (name: string): PackageExportSelection => ({
    kind: 'VOLUME',
    contentId: `volume-${name}`,
    partitionIndex: 0,
    volumeName: name,
    name,
    typeLabel: 'Volume',
});

describe('package export filenames', () => {
    it.each([
        [[volume('Volume A'), volume('Volume B')], 'Volume A and others.axkvol'],
        [[object('PROGRAM', 'Program A'), object('PROGRAM', 'Program B')], 'Program A and others.axkprg'],
        [[object('SEQU', 'Sequence A'), object('SEQU', 'Sequence B')], 'Sequence A and others.axkseq'],
        [[object('SBAC', 'Bank A'), object('SBAC', 'Bank B')], 'Bank A and others.axksbac'],
        [[object('SBNK', 'Sample A'), object('SBNK', 'Sample B')], 'Sample A and others.axksbnk'],
        [[object('SMPL', 'Wave A'), object('SMPL', 'Wave B')], 'Wave A and others.axksmpl'],
    ])('uses the typed extension for homogeneous roots', (items, expected) => {
        expect(packageExportFilename(items)).toBe(expected);
    });

    it('uses the sequence extension for a single Sequence', () => {
        expect(packageExportFilename([object('SEQU', 'Demo Song')])).toBe('Demo Song.axkseq');
    });

    it('uses the generic extension only for mixed root kinds', () => {
        expect(packageExportFilename([object('SBAC', 'Bank'), object('SBNK', 'Sample')])).toBe(
            'Bank and others.axkpkg',
        );
    });

    it.each([
        ['disks/Session.ima', 'Session.axkvol'],
        ['disks/Session.IMG', 'Session.axkvol'],
        ['disks/Session.backup.v1.ImA', 'Session.backup.v1.axkvol'],
    ])('uses the floppy image basename for a whole-root export from %s', (relativePath, expected) => {
        expect(
            packageExportFilename(
                [volume('FAT root')],
                serverFileLocation({ rootId: 'workspace', relativePath }, `Workspace/${relativePath}`),
            ),
        ).toBe(expected);
    });

    it('uses the source directory name for a whole AXK object-directory export', () => {
        expect(
            packageExportFilename(
                [volume('Object directory')],
                axkObjectDirectoryLocation(
                    { rootId: 'workspace', relativePath: 'floppies/FS1R/DISK2' },
                    'Workspace/floppies/FS1R/DISK2',
                ),
            ),
        ).toBe('DISK2.axkvol');
    });

    it('keeps the synthetic root label when the source has no usable basename', () => {
        expect(
            packageExportFilename(
                [volume('Object directory')],
                axkObjectDirectoryLocation({ rootId: 'workspace', relativePath: '' }, ''),
            ),
        ).toBe('Object directory.axkvol');
        expect(
            packageExportFilename(
                [volume('FAT root')],
                serverFileLocation({ rootId: 'workspace', relativePath: '.ima' }, '.ima'),
            ),
        ).toBe('FAT root.axkvol');
    });

    it('does not replace names for normal volumes, object selections, or multiple roots', () => {
        const floppy = serverFileLocation(
            { rootId: 'workspace', relativePath: 'floppies/Demo.ima' },
            'Workspace/floppies/Demo.ima',
        );
        expect(
            packageExportFilename(
                [volume('Piano')],
                serverFileLocation({ rootId: 'workspace', relativePath: 'disk.hds' }),
            ),
        ).toBe('Piano.axkvol');
        expect(packageExportFilename([object('SBNK', 'Kick')], floppy)).toBe('Kick.axksbnk');
        expect(packageExportFilename([volume('FAT root'), volume('Other root')], floppy)).toBe(
            'FAT root and others.axkvol',
        );
    });
});
