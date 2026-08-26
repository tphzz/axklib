import { describe, expect, it, vi } from 'vitest';
import { collectionPageStep, linearNavigationIndex, revealCollectionObject } from './collectionNavigation';

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

describe('revealCollectionObject', () => {
    it('scrolls a virtual collection before focusing the requested object', async () => {
        const workspace = document.createElement('main');
        const list = document.createElement('div');
        list.dataset.collectionList = 'samples';
        Object.defineProperty(list, 'clientHeight', { value: 100 });
        list.scrollTop = 0;
        const target = document.createElement('button');
        target.dataset.collectionObjectId = 'sample:[1]';
        const focus = vi.spyOn(target, 'focus');
        list.addEventListener('scroll', () => list.append(target), { once: true });
        workspace.append(list);

        expect(await revealCollectionObject(workspace, 'samples', 'sample:[1]', 10, 26)).toBe(true);
        expect(list.scrollTop).toBe(186);
        expect(focus).toHaveBeenCalledWith({ preventScroll: true });
    });

    it('centers an inspector relationship target without changing nearest-edge reveals', async () => {
        const workspace = document.createElement('main');
        const list = document.createElement('div');
        list.dataset.collectionList = 'samples';
        Object.defineProperty(list, 'clientHeight', { value: 100 });
        Object.defineProperty(list, 'scrollHeight', { value: 520 });
        list.scrollTop = 0;
        const target = document.createElement('button');
        target.dataset.collectionObjectId = 'sample:[1]';
        const focus = vi.spyOn(target, 'focus');
        list.addEventListener('scroll', () => list.append(target), { once: true });
        workspace.append(list);

        expect(await revealCollectionObject(workspace, 'samples', 'sample:[1]', 10, 26, 'center')).toBe(true);
        expect(list.scrollTop).toBe(223);
        expect(focus).toHaveBeenCalledWith({ preventScroll: true });
    });

    it('reapplies a virtual scroll after the collection receives its browser layout', async () => {
        const workspace = document.createElement('main');
        const list = document.createElement('div');
        list.dataset.collectionList = 'wave-data';
        let laidOut = false;
        Object.defineProperty(list, 'clientHeight', { get: () => (laidOut ? 126 : 0) });
        Object.defineProperty(list, 'scrollHeight', { get: () => (laidOut ? 4_200 : 0) });
        list.scrollTop = 0;
        const target = document.createElement('button');
        target.dataset.collectionObjectId = 'wave:[80]';
        const focus = vi.spyOn(target, 'focus');
        list.addEventListener('scroll', () => {
            if (laidOut && list.scrollTop === 3_276) list.append(target);
        });
        const requestAnimationFrame = vi.spyOn(window, 'requestAnimationFrame').mockImplementation((callback) => {
            laidOut = true;
            callback(0);
            return 1;
        });
        workspace.append(list);

        expect(await revealCollectionObject(workspace, 'wave-data', 'wave:[80]', 79, 42, 'center')).toBe(true);
        expect(requestAnimationFrame).toHaveBeenCalledOnce();
        expect(list.scrollTop).toBe(3_276);
        expect(focus).toHaveBeenCalledWith({ preventScroll: true });

        requestAnimationFrame.mockRestore();
    });

    it('does not claim success for an absent collection or object', async () => {
        const workspace = document.createElement('main');

        expect(await revealCollectionObject(workspace, 'samples', 'missing', 0, 26)).toBe(false);
    });
});
