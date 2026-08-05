import { describe, expect, it } from 'vitest';
import type { PackageExportObject } from './types';
import {
    emptyPackageExportSelection,
    maximumPackageExportRoots,
    updatePackageExportSelection,
} from './objectSelection';

describe('object selection', () => {
    const object = (
        objectId: string,
        kind: PackageExportObject['kind'],
        partitionIndex: number,
        volumeName: string,
        name: string,
    ) =>
        ({
            kind,
            objectId,
            name,
            typeLabel:
                kind === 'PROGRAM'
                    ? 'Program'
                    : kind === 'SBAC'
                      ? 'Sample Bank'
                      : kind === 'SBNK'
                        ? 'Sample'
                        : 'Wave Data',
            partitionIndex,
            partitionName: `Partition ${partitionIndex}`,
            volumeName,
        }) as PackageExportObject;

    it('accumulates mixed roots across lists and orders the same set deterministically', () => {
        const program = object('program', 'PROGRAM', 1, 'Later', 'Program');
        const sampleBank = object('bank', 'SBAC', 0, 'First', 'Bank');
        const sample = object('sample', 'SBNK', 0, 'First', 'Sample');
        const waveData = object('wave', 'SMPL', 0, 'First', 'Wave');

        let selection = emptyPackageExportSelection();
        selection = updatePackageExportSelection(
            selection,
            'programs',
            [program],
            [program],
            program.objectId,
            'toggle',
        ).selection;
        selection = updatePackageExportSelection(
            selection,
            'sample-banks',
            [sampleBank],
            [sampleBank],
            sampleBank.objectId,
            'toggle',
        ).selection;
        selection = updatePackageExportSelection(
            selection,
            'samples',
            [sample],
            [sample],
            sample.objectId,
            'toggle',
        ).selection;
        selection = updatePackageExportSelection(
            selection,
            'wave-data',
            [waveData],
            [waveData],
            waveData.objectId,
            'toggle',
        ).selection;

        expect(selection.items.map((item) => item.objectId)).toEqual(['bank', 'sample', 'wave', 'program']);
        expect(selection.anchors).toEqual({
            programs: 'program',
            'sample-banks': 'bank',
            samples: 'sample',
            'wave-data': 'wave',
        });
    });

    it('keeps other lists selected when replacing a range or selecting all visible rows', () => {
        const program = object('program', 'PROGRAM', 0, 'Volume', 'Program');
        const samples = [
            object('sample-a', 'SBNK', 0, 'Volume', 'A'),
            object('sample-b', 'SBNK', 0, 'Volume', 'B'),
            object('sample-c', 'SBNK', 0, 'Volume', 'C'),
        ];
        let selection = updatePackageExportSelection(
            emptyPackageExportSelection(),
            'programs',
            [program],
            [program],
            program.objectId,
            'toggle',
        ).selection;
        selection = updatePackageExportSelection(
            selection,
            'samples',
            samples,
            samples,
            samples[0]!.objectId,
            'toggle',
        ).selection;
        selection = updatePackageExportSelection(
            selection,
            'samples',
            samples,
            samples,
            samples[2]!.objectId,
            'range',
        ).selection;

        expect(selection.items.map((item) => item.objectId)).toEqual(['program', 'sample-a', 'sample-b', 'sample-c']);
        expect(selection.anchors.samples).toBe('sample-a');

        const visible = [samples[1]!];
        const all = updatePackageExportSelection(
            selection,
            'filtered-samples',
            samples,
            visible,
            samples[1]!.objectId,
            'all',
        ).selection;
        expect(all.items.map((item) => item.objectId)).toEqual(['program', 'sample-a', 'sample-b', 'sample-c']);
    });

    it('removes each exact toggle target from an existing range', () => {
        const samples = [
            object('sample-a', 'SBNK', 0, 'Volume', 'A'),
            object('sample-b', 'SBNK', 0, 'Volume', 'B'),
            object('sample-c', 'SBNK', 0, 'Volume', 'C'),
            object('sample-d', 'SBNK', 0, 'Volume', 'D'),
        ];
        let selection = updatePackageExportSelection(
            emptyPackageExportSelection(),
            'samples',
            samples,
            samples,
            samples[0]!.objectId,
            'replace',
        ).selection;
        selection = updatePackageExportSelection(
            selection,
            'samples',
            samples,
            samples,
            samples[3]!.objectId,
            'range',
        ).selection;
        selection = updatePackageExportSelection(
            selection,
            'samples',
            samples,
            samples,
            samples[1]!.objectId,
            'toggle',
        ).selection;

        expect(selection.items.map((item) => item.objectId)).toEqual(['sample-a', 'sample-c', 'sample-d']);

        selection = updatePackageExportSelection(
            selection,
            'samples',
            samples,
            samples,
            samples[2]!.objectId,
            'toggle',
        ).selection;

        expect(selection.items.map((item) => item.objectId)).toEqual(['sample-a', 'sample-d']);
    });

    it('deduplicates objects exposed through multiple relationship views', () => {
        const sample = object('sample', 'SBNK', 0, 'Volume', 'Sample');
        let selection = updatePackageExportSelection(
            emptyPackageExportSelection(),
            'standalone-samples',
            [sample],
            [sample],
            sample.objectId,
            'toggle',
        ).selection;
        selection = updatePackageExportSelection(
            selection,
            'bank-members',
            [sample],
            [sample],
            sample.objectId,
            'add-range',
        ).selection;

        expect(selection.items).toEqual([sample]);
    });

    it('rejects an over-limit update atomically instead of truncating it', () => {
        const items = Array.from({ length: maximumPackageExportRoots }, (_, index) =>
            object(`wave-${index}`, 'SMPL', 0, 'Volume', `Wave ${index}`),
        );
        const selection = {
            items,
            anchors: {},
        };
        const extra = object('extra', 'SMPL', 0, 'Volume', 'Extra');
        const update = updatePackageExportSelection(
            selection,
            'wave-data',
            [...items, extra],
            [...items, extra],
            extra.objectId,
            'toggle',
        );

        expect(update.limitExceeded).toBe(true);
        expect(update.selection).toBe(selection);
        expect(update.selection.items).toHaveLength(maximumPackageExportRoots);
    });
});
