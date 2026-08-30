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
    it('materializes a virtual collection once before revealing the requested object', async () => {
        const workspace = document.createElement('main');
        const list = document.createElement('div');
        list.dataset.collectionList = 'samples';
        Object.defineProperty(list, 'clientHeight', { value: 100 });
        list.scrollTop = 0;
        const target = document.createElement('button');
        target.dataset.collectionObjectId = 'sample:[1]';
        const scrollIntoView = vi.fn();
        target.scrollIntoView = scrollIntoView;
        const focus = vi.spyOn(target, 'focus');
        const onscroll = vi.fn(() => list.append(target));
        list.addEventListener('scroll', onscroll, { once: true });
        workspace.append(list);

        expect(await revealCollectionObject(workspace, 'samples', 'sample:[1]', 10, 26)).toBe(true);
        expect(list.scrollTop).toBe(186);
        expect(onscroll).toHaveBeenCalledOnce();
        expect(scrollIntoView).toHaveBeenCalledOnce();
        expect(scrollIntoView).toHaveBeenCalledWith({ block: 'nearest', inline: 'nearest' });
        expect(focus).not.toHaveBeenCalled();
    });

    it('centers an inspector relationship target with one native reveal and no correction frames', async () => {
        const workspace = document.createElement('main');
        const list = document.createElement('div');
        list.dataset.collectionList = 'samples';
        Object.defineProperty(list, 'clientHeight', { value: 100 });
        Object.defineProperty(list, 'scrollHeight', { value: 520 });
        list.scrollTop = 0;
        const target = document.createElement('button');
        target.dataset.collectionObjectId = 'sample:[1]';
        const scrollIntoView = vi.fn();
        target.scrollIntoView = scrollIntoView;
        const focus = vi.spyOn(target, 'focus');
        list.addEventListener('scroll', () => list.append(target), { once: true });
        const requestAnimationFrame = vi.spyOn(window, 'requestAnimationFrame');
        workspace.append(list);

        expect(await revealCollectionObject(workspace, 'samples', 'sample:[1]', 10, 26, 'center')).toBe(true);
        expect(list.scrollTop).toBe(223);
        expect(scrollIntoView).toHaveBeenCalledOnce();
        expect(scrollIntoView).toHaveBeenCalledWith({ block: 'center', inline: 'nearest' });
        expect(focus).not.toHaveBeenCalled();
        expect(requestAnimationFrame).not.toHaveBeenCalled();

        requestAnimationFrame.mockRestore();
    });

    it('focuses keyboard-activated relationship targets before the final native centering', async () => {
        const workspace = document.createElement('main');
        const list = document.createElement('div');
        list.dataset.collectionList = 'samples';
        Object.defineProperties(list, {
            clientHeight: { value: 100 },
            scrollHeight: { value: 520 },
        });
        list.scrollTop = 0;
        const target = document.createElement('button');
        target.dataset.collectionObjectId = 'sample:[1]';
        const scrollIntoView = vi.fn();
        target.scrollIntoView = scrollIntoView;
        const focus = vi.spyOn(target, 'focus');
        list.addEventListener('scroll', () => list.append(target), { once: true });
        workspace.append(list);

        expect(await revealCollectionObject(workspace, 'samples', 'sample:[1]', 10, 26, 'center', true)).toBe(true);
        expect(list.scrollTop).toBe(223);
        expect(focus).toHaveBeenCalledWith({ preventScroll: true });
        expect(scrollIntoView).toHaveBeenCalledWith({ block: 'center', inline: 'nearest' });
        expect(focus.mock.invocationCallOrder[0]).toBeLessThan(scrollIntoView.mock.invocationCallOrder[0]!);
    });

    it('does not claim success for an absent collection or object', async () => {
        const workspace = document.createElement('main');

        expect(await revealCollectionObject(workspace, 'samples', 'missing', 0, 26)).toBe(false);
    });
});
