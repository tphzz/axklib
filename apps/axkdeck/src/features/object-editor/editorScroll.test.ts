import { tick } from 'svelte';
import { describe, expect, it } from 'vitest';
import { rememberEditorScroll } from './editorScroll';

describe('Editor page scroll', () => {
    it('retains separate page positions across document remounts and removes listeners', async () => {
        const positions = new Map<string, { top: number; left: number }>();
        const node = document.createElement('div');
        const action = rememberEditorScroll(node, { positions, key: 'control' });
        await tick();
        node.scrollTop = 120;
        node.dispatchEvent(new Event('scroll'));
        action.update({ positions, key: 'pitch' });
        await tick();
        expect(node.scrollTop).toBe(0);
        action.update({ positions, key: 'control' });
        await tick();
        expect(node.scrollTop).toBe(120);
        action.destroy();
        node.scrollTop = 0;
        node.dispatchEvent(new Event('scroll'));
        expect(positions.get('control')?.top).toBe(120);
        const next = rememberEditorScroll(node, { positions, key: 'control' });
        await tick();
        expect(node.scrollTop).toBe(120);
        next.destroy();
    });
});
