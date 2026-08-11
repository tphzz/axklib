import { describe, expect, it, vi } from 'vitest';

import { collectPages } from './pagination';

describe('collectPages', () => {
    it('collects stable pages with unique forward progress', async () => {
        const fetchPage = vi
            .fn()
            .mockResolvedValueOnce({ items: [{ id: 'one' }], totalCount: 2 })
            .mockResolvedValueOnce({ items: [{ id: 'two' }], totalCount: 2 });

        await expect(collectPages<{ id: string }>(fetchPage, { key: (item) => item.id })).resolves.toEqual([
            { id: 'one' },
            { id: 'two' },
        ]);
        expect(fetchPage).toHaveBeenNthCalledWith(2, 1, 256);
    });

    it.each([
        [{ items: [], totalCount: 1 }, 'made no forward progress'],
        [{ items: [{ id: 'one' }, { id: 'one' }], totalCount: 2 }, 'repeated an item'],
        [{ items: [{ id: 'one' }], totalCount: -1 }, 'invalid pagination total'],
    ])('rejects an inconsistent page %#', async (page, message) => {
        await expect(collectPages(async () => page, { key: (item) => item.id })).rejects.toThrow(message);
    });

    it('rejects a changing total and excessive page count', async () => {
        const changing = vi
            .fn()
            .mockResolvedValueOnce({ items: [{ id: 'one' }], totalCount: 2 })
            .mockResolvedValueOnce({ items: [{ id: 'two' }], totalCount: 3 });
        await expect(collectPages<{ id: string }>(changing, { key: (item) => item.id })).rejects.toThrow(
            'total changed',
        );

        let page = 0;
        await expect(
            collectPages(async () => ({ items: [{ id: String(page++) }], totalCount: 3 }), {
                key: (item) => item.id,
                maximumPages: 2,
            }),
        ).rejects.toThrow('request limit');
    });
});
