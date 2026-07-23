import { describe, expect, it, vi } from 'vitest';

import { modal } from './modal';

describe('modal', () => {
    it('owns focus, traps tab navigation, handles Escape, and restores background state', async () => {
        const background = document.createElement('button');
        const dialog = document.createElement('div');
        const first = document.createElement('button');
        const last = document.createElement('button');
        dialog.append(first, last);
        document.body.append(background, dialog);
        background.focus();
        const onescape = vi.fn();

        const action = modal(dialog, { onescape });
        await Promise.resolve();
        expect(document.activeElement).toBe(first);
        expect(background.inert).toBe(true);

        last.focus();
        last.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', bubbles: true, cancelable: true }));
        expect(document.activeElement).toBe(first);
        dialog.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
        expect(onescape).toHaveBeenCalledOnce();

        action.destroy();
        expect(background.inert).toBe(false);
        expect(document.activeElement).toBe(background);
        background.remove();
        dialog.remove();
    });
});
