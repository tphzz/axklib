import { describe, expect, it } from 'vitest';

import { collectionPageStep, linearNavigationIndex } from './collectionNavigation';

describe('collectionNavigation', () => {
    it('uses one row of overlap when calculating a visible page step', () => {
        const list = document.createElement('div');
        const row = document.createElement('button');
        list.dataset.navigationList = '';
        list.append(row);
        document.body.append(list);
        Object.defineProperty(list, 'clientHeight', { configurable: true, value: 210 });

        expect(collectionPageStep(row, 42)).toBe(4);

        list.remove();
    });

    it('pages repeatedly in either direction and clamps to the list boundaries', () => {
        expect(linearNavigationIndex('PageDown', 0, 80, 4)).toBe(4);
        expect(linearNavigationIndex('PageDown', 4, 80, 4)).toBe(8);
        expect(linearNavigationIndex('PageUp', 8, 80, 4)).toBe(4);
        expect(linearNavigationIndex('PageUp', 2, 80, 4)).toBe(0);
        expect(linearNavigationIndex('PageDown', 78, 80, 4)).toBe(79);
    });

    it('does not add paging to callers that omit an explicit page step', () => {
        expect(linearNavigationIndex('PageDown', 0, 80)).toBeNull();
        expect(linearNavigationIndex('PageUp', 40, 80)).toBeNull();
    });
});
