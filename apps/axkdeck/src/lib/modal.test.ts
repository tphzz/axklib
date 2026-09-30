/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { describe, expect, it, vi } from 'vitest';
import { fireEvent, render, within } from '@testing-library/svelte';
import ImportDestinationChooser from './components/ImportDestinationChooser.svelte';

import { modal } from './modal';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

describe('modal', () => {
    it('does not let a synchronously closed nested Escape dismiss its parent as well', async () => {
        const parent = document.createElement('div');
        const child = document.createElement('div');
        const trigger = document.createElement('button');
        parent.append(trigger, child);
        document.body.append(parent);
        const onescape = vi.fn();
        const first = modal(parent, { onescape });
        const second = modal(child, { onescape: () => second.destroy() });
        try {
            child.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
            expect(onescape).not.toHaveBeenCalled();
            await Promise.resolve();
            expect(document.activeElement).toBe(trigger);
        } finally {
            second.destroy();
            first.destroy();
            parent.remove();
        }
    });
    it('layers the newest child above a raised parent and restores the original layer', () => {
        const parent = document.createElement('div');
        parent.className = 'dialog-backdrop dialog-backdrop-raised';
        const child = document.createElement('div');
        child.className = 'dialog-backdrop';
        child.style.setProperty('--modal-layer', '17', 'important');
        document.body.append(parent, child);
        const first = modal(parent);
        const second = modal(child);
        try {
            expect(parent.style.getPropertyValue('--modal-layer')).toBe('50');
            expect(Number(child.style.getPropertyValue('--modal-layer'))).toBeGreaterThan(
                Number(parent.style.getPropertyValue('--modal-layer')),
            );
        } finally {
            second.destroy();
            first.destroy();
            expect(child.style.getPropertyValue('--modal-layer')).toBe('17');
            expect(child.style.getPropertyPriority('--modal-layer')).toBe('important');
            parent.remove();
            child.remove();
        }
    });

    it('releases an old inert ancestor and prevents background observers from stealing child focus', async () => {
        const host = document.createElement('div');
        const parent = document.createElement('div');
        const input = document.createElement('input');
        input.dataset.dialogInitialFocus = 'select';
        input.disabled = true;
        parent.append(input);
        document.body.append(host, parent);
        const first = modal(parent);
        const child = document.createElement('div');
        const button = document.createElement('button');
        child.append(button);
        host.append(child);
        const second = modal(child);
        try {
            await Promise.resolve();
            expect(host.inert).toBeFalsy();
            expect(document.activeElement).toBe(button);
            input.disabled = false;
            await Promise.resolve();
            expect(document.activeElement).toBe(button);
        } finally {
            second.destroy();
            first.destroy();
            parent.remove();
            host.remove();
        }
    });

    it('keeps top focus when an underlying modal is destroyed out of order', async () => {
        const existing = document.createElement('div');
        existing.inert = true;
        const nodes = Array.from({ length: 3 }, () => {
            const node = document.createElement('div');
            node.append(document.createElement('button'));
            return node;
        });
        document.body.append(existing, ...nodes);
        const actions = [];
        for (const node of nodes) {
            actions.push(modal(node));
            await Promise.resolve();
        }
        try {
            await Promise.resolve();
            const focused = nodes[2].firstElementChild;
            expect(document.activeElement).toBe(focused);
            actions[0].destroy();
            nodes[0].remove();
            expect(document.activeElement).toBe(focused);
            expect(nodes[1].inert).toBe(true);
            actions[1].destroy();
            nodes[1].remove();
            expect(document.activeElement).toBe(focused);
        } finally {
            actions[2].destroy();
            expect(existing.inert).toBe(true);
            existing.remove();
            nodes.forEach((node) => node.remove());
        }
    });

    it('lets an expanded destination chooser consume Escape before dismissing its dialog', async () => {
        const dialog = document.createElement('div');
        document.body.append(dialog);
        const onescape = vi.fn();
        const action = modal(dialog, { onescape });
        const component = render(ImportDestinationChooser, {
            target: dialog,
            props: {
                mode: 'existing',
                partitionIndex: 0,
                volumeName: 'Samples',
                partitions: [{ partitionIndex: 0, name: 'Partition' }],
                volumes: [
                    { partitionIndex: 0, name: 'Partition', volumeName: 'Samples', label: 'Partition / Samples' },
                ],
                disabled: false,
                onmode: vi.fn(),
                onvolume: vi.fn(),
                onpartition: vi.fn(),
                onname: vi.fn(),
            },
        });
        try {
            const input = within(dialog).getByRole('combobox', { name: 'Destination volume' });
            await fireEvent.click(input);
            expect(input.getAttribute('aria-expanded')).toBe('true');
            await fireEvent.keyDown(input, { key: 'Escape' });
            expect(input.getAttribute('aria-expanded')).toBe('false');
            expect(onescape).not.toHaveBeenCalled();
            await fireEvent.keyDown(input, { key: 'Escape' });
            expect(onescape).toHaveBeenCalledOnce();
        } finally {
            component.unmount();
            action.destroy();
            dialog.remove();
        }
    });

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

    it('preserves scrollbar geometry in inert modal backgrounds', () => {
        expect(appStyles).toMatch(/\*\s*\{[^}]*scrollbar-width:\s*thin;[^}]*\}/);
        expect(appStyles).toMatch(/\.modal-scrollbar-measurement\s*\{[^}]*scrollbar-gutter:\s*stable;/);
        expect(appStyles).not.toMatch(/^:is\(\[inert\], \[inert\] \*\)\s*\{[^}]*scrollbar-width:\s*none;[^}]*\}/m);
        expect(appStyles).not.toMatch(
            /^:is\(\[inert\], \[inert\] \*\)::-webkit-scrollbar\s*\{[^}]*display:\s*none;[^}]*\}/m,
        );
    });

    it.each([0, 10])('only removes zero-gutter overlay scrollbars (gutter %i)', (gutter) => {
        const width = vi.spyOn(HTMLElement.prototype, 'offsetWidth', 'get').mockReturnValue(100);
        const client = vi.spyOn(HTMLElement.prototype, 'clientWidth', 'get').mockReturnValue(100 - gutter);
        const parent = document.createElement('div');
        const child = document.createElement('div');
        document.body.append(parent);
        const first = modal(parent);
        document.body.append(child);
        const second = modal(child);
        try {
            expect(document.documentElement.classList.contains('modal-overlay-scrollbars')).toBe(gutter === 0);
            second.destroy();
            expect(document.documentElement.classList.contains('modal-overlay-scrollbars')).toBe(gutter === 0);
            first.destroy();
            expect(document.documentElement.classList.contains('modal-overlay-scrollbars')).toBe(false);
            expect(document.querySelector('.modal-scrollbar-measurement')).toBeNull();
        } finally {
            parent.remove();
            child.remove();
            width.mockRestore();
            client.mockRestore();
        }
    });

    it('suppresses background scrollbar painting without removing scrollbar geometry', () => {
        expect(appStyles).toMatch(
            /:is\(\[inert\], \[inert\] \*\)\s*\{[^}]*scrollbar-color:\s*transparent transparent;/,
        );
        for (const part of ['thumb', 'track', 'track-piece', 'button', 'corner']) {
            expect(appStyles).toContain(`:is([inert], [inert] *)::-webkit-scrollbar-${part}`);
        }
        expect(appStyles).toMatch(/::-webkit-scrollbar-corner\s*\{[^}]*visibility:\s*hidden;/);
    });

    it('keeps the background inert until the last nested modal releases it', async () => {
        const background = document.createElement('main');
        const trigger = document.createElement('button');
        background.append(trigger);
        const parent = document.createElement('div');
        const nestedTrigger = document.createElement('button');
        parent.append(nestedTrigger);
        document.body.append(background, parent);
        trigger.focus();
        const parentAction = modal(parent);
        await Promise.resolve();
        const child = document.createElement('div');
        document.body.append(child);
        const childAction = modal(child);
        await Promise.resolve();
        expect(background.inert).toBe(true);
        expect(parent.inert).toBe(true);
        expect(child.inert).toBeFalsy();
        childAction.destroy();
        child.remove();
        expect(parent.inert).toBe(false);
        expect(background.inert).toBe(true);
        expect(document.activeElement).toBe(nestedTrigger);
        parentAction.destroy();
        expect(background.inert).toBe(false);
        expect(document.activeElement).toBe(trigger);
        background.remove();
        parent.remove();
    });
});
