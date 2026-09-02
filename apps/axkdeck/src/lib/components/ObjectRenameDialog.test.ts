/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { SamplerObject } from '../transport';
import ObjectRenameDialog from './ObjectRenameDialog.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

const object: SamplerObject = {
    key: 'object-1',
    objectType: 'PROG',
    name: '001',
    partitionIndex: 0,
    partitionName: 'PARTITION 1',
    volumeName: 'Volume',
    categoryName: 'PROG',
    objectEncoding: 'current',
    directoryEntryName: 'PROG.001',
    sfsId: 8,
    storedSizeBytes: 912,
    sizeWithDependenciesBytes: null,
    sampleRate: 0,
    rootKey: 0,
    storedFrameCount: 0,
    waveStartFrame: 0,
    waveLengthFrames: 0,
    storageState: 'COMPLETE',
    sampleWidthBytes: 0,
};

describe('ObjectRenameDialog', () => {
    it('enforces the eight-character Program display-name limit', async () => {
        const onsubmit = vi.fn();
        render(ObjectRenameDialog, {
            props: {
                target: { kind: 'program', object, name: 'Pgm 001', programNumber: 1 },
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        const input = screen.getByLabelText('Program name');
        const submit = screen.getByRole('button', { name: 'Rename' });
        expect((input as HTMLInputElement).value).toBe('Pgm 001');
        expect((input as HTMLInputElement).maxLength).toBe(8);
        expect((submit as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.input(input, { target: { value: 'New Name' } });
        await fireEvent.click(submit);
        expect(onsubmit).toHaveBeenCalledWith('New Name');
    });

    it('uses the sixteen-character object-name limit and locks dismissal while busy', () => {
        render(ObjectRenameDialog, {
            props: {
                target: { kind: 'sample-bank', object: { ...object, objectType: 'SBAC' }, name: 'Bank' },
                busy: true,
                error: 'Rename failed',
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        expect((screen.getByLabelText('Sample Bank name') as HTMLInputElement).maxLength).toBe(16);
        expect((screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: 'Renaming' }) as HTMLButtonElement).disabled).toBe(true);
        expect(screen.getByRole('alert').textContent).toContain('Rename failed');
    });

    it('uses Sequence terminology for SEQU renames', () => {
        render(ObjectRenameDialog, {
            props: {
                target: { kind: 'sequence', object: { ...object, objectType: 'SEQU' }, name: 'Demo Song' },
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        expect(screen.getByRole('dialog', { name: 'Rename Sequence' })).toBeTruthy();
        expect((screen.getByLabelText('Sequence name') as HTMLInputElement).maxLength).toBe(16);
    });

    it('keeps the primary and secondary footer actions aligned', () => {
        const actionGeometry = appStyles.match(
            /\.secondary-button,\s*\.primary-button,\s*\.danger-button\s*\{[^}]+\}/,
        )?.[0];
        const dialogActionGeometry = appStyles.match(
            /\.dialog-footer \.secondary-button,\s*\.dialog-footer \.primary-button,\s*\.dialog-footer \.danger-button\s*\{[^}]+\}/,
        )?.[0];
        expect(actionGeometry).toBeDefined();
        expect(dialogActionGeometry).toBeDefined();

        const style = document.createElement('style');
        style.textContent = `${actionGeometry}\n${dialogActionGeometry}`;
        document.head.append(style);

        render(ObjectRenameDialog, {
            props: {
                target: { kind: 'sample', object: { ...object, objectType: 'SBNK' }, name: 'Sample' },
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        const cancelStyle = getComputedStyle(screen.getByRole('button', { name: 'Cancel' }));
        const renameStyle = getComputedStyle(screen.getByRole('button', { name: 'Rename' }));
        expect(cancelStyle.height).toBe('30px');
        expect(renameStyle.height).toBe('30px');
        expect(cancelStyle.marginTop).toBe('0px');
        expect(renameStyle.marginTop).toBe('0px');
        expect(cancelStyle.marginBottom).toBe('0px');
        expect(renameStyle.marginBottom).toBe('0px');

        style.remove();
    });
});
