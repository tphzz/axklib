/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { describe, expect, it, vi } from 'vitest';

import { modal } from './modal';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

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

    it('selects explicitly marked prefilled text without making every dialog input the default', async () => {
        const dialog = document.createElement('div');
        const close = document.createElement('button');
        const input = document.createElement('input');
        input.value = 'Existing name';
        input.setAttribute('data-dialog-initial-focus', 'select');
        dialog.append(close, input);
        document.body.append(dialog);

        const select = vi.spyOn(input, 'select');
        const action = modal(dialog);
        await Promise.resolve();

        expect(document.activeElement).toBe(input);
        expect(select).toHaveBeenCalledOnce();

        action.destroy();
        dialog.remove();
    });

    it('suppresses native and WebKit overlay scrollbars in inert modal backgrounds', () => {
        expect(appStyles).toMatch(/:is\(\[inert\], \[inert\] \*\)\s*\{[^}]*scrollbar-width:\s*none;[^}]*\}/);
        expect(appStyles).toMatch(/:is\(\[inert\], \[inert\] \*\)::-webkit-scrollbar\s*\{[^}]*display:\s*none;[^}]*\}/);
    });
});
