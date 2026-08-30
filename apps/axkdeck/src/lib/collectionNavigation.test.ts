import { describe, expect, it, vi } from 'vitest';
import {
    collectionPageStep,
    focusCollectionIndex,
    linearNavigationIndex,
    revealCollectionObject,
} from './collectionNavigation';

describe('collectionNavigation', () => {
    it('uses one row of overlap when calculating a visible page step', () => {
        const list = document.createElement('div');
        const row = document.createElement('button');
        list.dataset.navigationList = '';
        list.append(row);
        document.body.append(list);
        Object.defineProperty(list, 'clientHeight', { configurable: true, value: 210 });
        Object.defineProperty(row, 'offsetHeight', { configurable: true, value: 42 });

        expect(collectionPageStep(row)).toBe(4);

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
    it('reveals an already-rendered object without writing or synthesizing scroll state', async () => {
        const workspace = document.createElement('main');
        const list = document.createElement('div');
        list.dataset.collectionList = 'samples';
        const setScrollTop = vi.fn();
        Object.defineProperty(list, 'scrollTop', { get: () => 0, set: setScrollTop });
        const target = document.createElement('button');
        target.dataset.collectionObjectId = 'sample:[1]';
        const scrollIntoView = vi.fn();
        target.scrollIntoView = scrollIntoView;
        const focus = vi.spyOn(target, 'focus');
        const onscroll = vi.fn();
        list.addEventListener('scroll', onscroll);
        list.append(target);
        workspace.append(list);

        expect(await revealCollectionObject(workspace, 'samples', 'sample:[1]')).toBe(true);
        expect(setScrollTop).not.toHaveBeenCalled();
        expect(onscroll).not.toHaveBeenCalled();
        expect(scrollIntoView).toHaveBeenCalledOnce();
        expect(scrollIntoView).toHaveBeenCalledWith({ block: 'nearest', inline: 'nearest', behavior: 'auto' });
        expect(focus).not.toHaveBeenCalled();
    });

    it('centers an inspector relationship target with one native reveal', async () => {
        const workspace = document.createElement('main');
        const list = document.createElement('div');
        list.dataset.collectionList = 'samples';
        const target = document.createElement('button');
        target.dataset.collectionObjectId = 'sample:[1]';
        const scrollIntoView = vi.fn();
        target.scrollIntoView = scrollIntoView;
        const focus = vi.spyOn(target, 'focus');
        const requestAnimationFrame = vi.spyOn(window, 'requestAnimationFrame');
        list.append(target);
        workspace.append(list);

        expect(await revealCollectionObject(workspace, 'samples', 'sample:[1]', 'center')).toBe(true);
        expect(scrollIntoView).toHaveBeenCalledOnce();
        expect(scrollIntoView).toHaveBeenCalledWith({ block: 'center', inline: 'nearest', behavior: 'auto' });
        expect(focus).not.toHaveBeenCalled();
        expect(requestAnimationFrame).not.toHaveBeenCalled();

        requestAnimationFrame.mockRestore();
    });

    it('focuses keyboard-activated relationship targets before the final native centering', async () => {
        const workspace = document.createElement('main');
        const list = document.createElement('div');
        list.dataset.collectionList = 'samples';
        const target = document.createElement('button');
        target.dataset.collectionObjectId = 'sample:[1]';
        const scrollIntoView = vi.fn();
        target.scrollIntoView = scrollIntoView;
        const focus = vi.spyOn(target, 'focus');
        list.append(target);
        workspace.append(list);

        expect(await revealCollectionObject(workspace, 'samples', 'sample:[1]', 'center', true)).toBe(true);
        expect(focus).toHaveBeenCalledWith({ preventScroll: true });
        expect(scrollIntoView).toHaveBeenCalledWith({ block: 'center', inline: 'nearest', behavior: 'auto' });
        expect(focus.mock.invocationCallOrder[0]).toBeLessThan(scrollIntoView.mock.invocationCallOrder[0]!);
    });

    it('does not claim success for an absent collection or object', async () => {
        const workspace = document.createElement('main');

        expect(await revealCollectionObject(workspace, 'samples', 'missing')).toBe(false);
    });
});

describe('focusCollectionIndex', () => {
    it('focuses and reveals a rendered keyboard target without assigning scrollTop', async () => {
        const workspace = document.createElement('main');
        workspace.dataset.navigationWorkspace = '';
        const list = document.createElement('div');
        list.dataset.navigationList = '';
        const setScrollTop = vi.fn();
        Object.defineProperty(list, 'scrollTop', { get: () => 0, set: setScrollTop });
        const current = document.createElement('button');
        current.dataset.navigationIndex = '0';
        const target = document.createElement('button');
        target.dataset.navigationIndex = '1';
        target.scrollIntoView = vi.fn();
        list.append(current, target);
        workspace.append(list);
        document.body.append(workspace);

        await focusCollectionIndex(current, 1);

        expect(document.activeElement).toBe(target);
        expect(setScrollTop).not.toHaveBeenCalled();
        expect(target.scrollIntoView).toHaveBeenCalledWith({
            block: 'nearest',
            inline: 'nearest',
            behavior: 'auto',
        });
        workspace.remove();
    });
});
