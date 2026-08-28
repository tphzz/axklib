/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import CreateSampleBankDialog from './CreateSampleBankDialog.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

describe('CreateSampleBankDialog', () => {
    it('focuses the name, rejects case-insensitive duplicates, and submits a valid name', async () => {
        const onsubmit = vi.fn();
        render(CreateSampleBankDialog, {
            props: {
                volumeName: 'Keys',
                sampleCount: 3,
                assignedSampleCount: 2,
                existingNames: ['Piano Bank'],
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        expect(
            screen.getByText(
                '2 selected Samples are already assigned. They will be detached from their current Sample Banks.',
            ),
        ).toBeTruthy();

        const name = screen.getByRole('textbox', { name: 'Sample Bank name' });
        await waitFor(() => expect(document.activeElement).toBe(name));
        expect(name.classList).toContain('dialog-field-control');

        const controlRule = appStyles.match(/\.dialog-field-control\s*\{[^}]+\}/)?.[0];
        if (!controlRule) throw new Error('Shared dialog field control rule is missing');
        expect(controlRule).toContain('background: var(--color-bg-deep)');

        const style = document.createElement('style');
        style.textContent = controlRule
            .replaceAll('var(--density-control)', '26px')
            .replaceAll('var(--dialog-control-font-size)', '11px')
            .replaceAll('var(--color-bg-deep)', '#0b0d0f');
        document.head.append(style);

        const controlStyle = getComputedStyle(name);
        expect(controlStyle.height).toBe('26px');
        expect(controlStyle.fontSize).toBe('11px');
        expect(controlStyle.backgroundColor).toBe('rgb(11, 13, 15)');
        style.remove();

        await fireEvent.input(name, { target: { value: 'piano bank' } });
        expect(screen.getByText('Sample Bank already exists: piano bank')).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Create Sample Bank' }) as HTMLButtonElement).disabled).toBe(true);

        await fireEvent.input(name, { target: { value: 'Layered Keys' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Create Sample Bank' }));
        expect(onsubmit).toHaveBeenCalledWith('Layered Keys');
    });
});
