import { afterEach, describe, expect, it } from 'vitest';
import { retainWorkspaceScroll, shellViewState } from './viewMemory';

const cleanup: (() => void)[] = [];
afterEach(() => {
    cleanup.splice(0).forEach((destroy) => destroy());
    document.body.replaceChildren();
});

function fixture(top = 20) {
    const node = document.createElement('div');
    const list = document.createElement('div');
    list.dataset.navigationList = '';
    list.setAttribute('aria-label', 'Performances');
    const inspector = document.createElement('aside');
    node.append(list, inspector);
    document.body.append(node);
    const state = shellViewState();
    state.scroll.set('Performances:0', { top, left: 0 });
    const start = () => cleanup.push(retainWorkspaceScroll(node, state).destroy);
    return { node, list, inspector, state, start };
}

const settle = () => new Promise<void>((resolve) => queueMicrotask(resolve));

describe('workspace scroll retention', () => {
    it('does not undo keyboard reveal before the resulting scroll event arrives', async () => {
        const { list, inspector, state, start } = fixture();
        start();
        expect(list.scrollTop).toBe(20);
        list.scrollTop = 280;
        inspector.append(document.createElement('p'));
        await settle();
        expect(list.scrollTop).toBe(280);
        list.dispatchEvent(new Event('scroll'));
        expect(state.scroll.get('Performances:0')).toEqual({ top: 280, left: 0 });
    });

    it('restores independent collections when their list element is reused', async () => {
        const { list, state, start } = fixture(120);
        state.scroll.set('Voices:0', { top: 40, left: 0 });
        start();
        list.setAttribute('aria-label', 'Voices');
        list.replaceChildren(document.createElement('button'));
        await settle();
        expect(list.scrollTop).toBe(40);
        list.scrollTop = 90;
        list.dispatchEvent(new Event('scroll'));
        list.setAttribute('aria-label', 'Performances');
        list.replaceChildren(document.createElement('button'));
        await settle();
        expect(list.scrollTop).toBe(120);
        expect(state.scroll.get('Voices:0')?.top).toBe(90);
        list.setAttribute('aria-label', 'Timbres');
        list.replaceChildren(document.createElement('button'));
        await settle();
        expect(list.scrollTop).toBe(0);
    });

    it('waits for asynchronous rows without overwriting the requested position with a clamped scroll event', async () => {
        const { list, state, start } = fixture(180);
        let maximum = 0;
        let top = 0;
        Object.defineProperty(list, 'scrollTop', {
            get: () => top,
            set: (value: number) => (top = Math.min(maximum, value)),
        });
        start();
        expect(list.scrollTop).toBe(0);
        list.dispatchEvent(new Event('scroll'));
        expect(state.scroll.get('Performances:0')?.top).toBe(180);
        maximum = 500;
        list.append(document.createElement('button'));
        await settle();
        expect(list.scrollTop).toBe(180);
    });

    it('lets navigation supersede a still-pending restoration', async () => {
        const { list, inspector, state, start } = fixture(180);
        let maximum = 60;
        let top = 0;
        Object.defineProperty(list, 'scrollTop', {
            get: () => top,
            set: (value: number) => (top = Math.min(maximum, value)),
        });
        start();
        expect(list.scrollTop).toBe(60);
        list.scrollTop = 30;
        maximum = 500;
        inspector.append(document.createElement('p'));
        await settle();
        expect(list.scrollTop).toBe(30);
        expect(state.scroll.get('Performances:0')?.top).toBe(30);
    });

    it('restores a remounted list but ignores later content updates on the live list', async () => {
        const { node, list, start } = fixture(120);
        start();
        const replacement = list.cloneNode() as HTMLElement;
        list.remove();
        node.prepend(replacement);
        await settle();
        expect(replacement.scrollTop).toBe(120);
        replacement.scrollTop = 200;
        replacement.append(document.createElement('button'));
        await settle();
        expect(replacement.scrollTop).toBe(200);
    });
});
