import { describe, expect, it } from 'vitest';
import { buildExportTree, visibleExportRows, exportTreePage } from './exportTree';

describe('Export tree', () => {
    const inspection = {
        imageId: 'image',
        revision: 1,
        rootDirectory: { entryId: 'root', sourcePath: '/AMENBOSH', name: 'AMENBOSH' },
        notices: [],
        totalBytes: 4,
        entries: [
            { entryId: 'dir', sourcePath: '/AMENBOSH/SUSP', relativePath: ['SUSP'], directory: true, sizeBytes: 0 },
            ...Array.from({ length: 205 }, (_, i) => ({
                entryId: String(i),
                sourcePath: `/AMENBOSH/SUSP/${i}`,
                relativePath: ['SUSP', String(i)],
                directory: false,
                sizeBytes: 4,
            })),
        ],
    };
    it('shows the destination root once and preserves ancestors across pages', () => {
        const root = buildExportTree(inspection, 'Renamed');
        const rows = visibleExportRows(root, new Set());
        expect(rows).toHaveLength(207);
        expect(rows[0].name).toBe('Renamed');
        expect(rows[2].path).toBe('Renamed/SUSP/0');
        const page = exportTreePage(rows, 1);
        expect(page).toHaveLength(102);
        expect(page.slice(0, 2).map((row) => row.name)).toEqual(['Renamed', 'SUSP']);
        expect(page[2]).toBe(rows[100]);
        expect(visibleExportRows(root, new Set([rows[1].id])).map((row) => row.name)).toEqual(['Renamed', 'SUSP']);
        expect(inspection.entries).toHaveLength(206);
    });
    it('represents an empty exported directory without manufacturing an archive entry', () => {
        const root = buildExportTree({ ...inspection, entries: [] }, 'Empty');
        expect(visibleExportRows(root, new Set()).map((row) => row.name)).toEqual(['Empty']);
    });

    it('retains deep paths and long names without recursive traversal or display-name identities', () => {
        const relativePath = [...Array.from({ length: 80 }, (_, i) => `Level ${i}`), 'x'.repeat(255)];
        const root = buildExportTree({ ...inspection, entries: [{ ...inspection.entries[1], relativePath }] }, 'Root');
        const rows = visibleExportRows(root, new Set());
        expect(rows).toHaveLength(82);
        expect(rows.at(-1)?.name).toBe('x'.repeat(255));
        expect(rows.at(-1)?.path).toBe(['Root', ...relativePath].join('/'));
        expect(rows.at(-1)?.depth).toBe(81);
        expect(visibleExportRows(root, new Set([rows[40].id]))).toHaveLength(41);
    });
});
