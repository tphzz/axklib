/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { ImageTransport } from '../transport';
import ServerStoragePicker from './ServerStoragePicker.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

function transport(withRoots = true): ImageTransport {
    return {
        storageMode: 'server',
        sandboxRoots: vi.fn().mockResolvedValue(
            withRoots
                ? [
                      { id: 'workspace', displayName: 'Yamaha images', writable: true },
                      { id: 'readonly', displayName: 'Archive', writable: false },
                  ]
                : [],
        ),
        sandboxDirectory: vi.fn().mockImplementation(async (directory) => ({
            directory,
            entries: directory.relativePath
                ? [{ name: 'nested.hds', relativePath: 'images/nested.hds', kind: 'FILE', size: 2048 }]
                : [
                      { name: 'images', relativePath: 'images', kind: 'DIRECTORY', size: null },
                      { name: 'disk.hds', relativePath: 'disk.hds', kind: 'FILE', size: 1024 },
                      { name: 'notes.txt', relativePath: 'notes.txt', kind: 'FILE', size: 20 },
                  ],
            truncated: false,
            nextCursor: null,
        })),
        createSandboxDirectory: vi.fn().mockResolvedValue(undefined),
    } as unknown as ImageTransport;
}

describe('ServerStoragePicker', () => {
    it('uses the task title without exposing server-filesystem terminology', async () => {
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Open image',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('dialog', { name: 'Open image' }).classList.contains('dialog-shell')).toBe(true);
        expect(screen.queryByText('Server filesystem')).toBeNull();
        expect(await screen.findByText('Yamaha images')).toBeTruthy();
    });

    it('restores and reports the directory supplied by its owner', async () => {
        const imageTransport = transport();
        const ondirectorychange = vi.fn();
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'file',
                title: 'Open image',
                extensions: ['hds'],
                initialDirectory: { rootId: 'workspace', relativePath: 'images' },
                ondirectorychange,
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(await screen.findByText('nested.hds')).toBeTruthy();
        expect(imageTransport.sandboxDirectory).toHaveBeenCalledWith({
            rootId: 'workspace',
            relativePath: 'images',
        });
        expect(ondirectorychange).toHaveBeenCalledWith({
            rootId: 'workspace',
            relativePath: 'images',
        });
    });

    it('returns an exact file reference and filters unrelated files', async () => {
        const onselect = vi.fn();
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Open image',
                extensions: ['hds'],
                onselect,
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        expect(screen.queryByText('notes.txt')).toBeNull();
        await fireEvent.click(await screen.findByText('disk.hds'));

        expect(onselect).toHaveBeenCalledWith({
            kind: 'server-file',
            reference: { rootId: 'workspace', relativePath: 'disk.hds' },
            displayName: 'Yamaha images/disk.hds',
        });
    });

    it('keeps the open-image picker read-only even in writable locations', async () => {
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Open image',
                extensions: ['hds'],
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        expect(screen.queryByRole('button', { name: 'New folder' })).toBeNull();
        expect(screen.queryByRole('button', { name: /More actions/ })).toBeNull();
        expect(screen.queryByText('Rename')).toBeNull();
        expect(screen.queryByText('Delete')).toBeNull();
    });

    it('creates a folder only in destination-selection workflows', async () => {
        const imageTransport = transport();
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'directory',
                title: 'Choose image location',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        await fireEvent.click(screen.getByRole('button', { name: 'New folder' }));
        await fireEvent.input(screen.getByLabelText('Folder name'), { target: { value: 'Created images' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Create' }));

        await waitFor(() =>
            expect(imageTransport.createSandboxDirectory).toHaveBeenCalledWith(
                { rootId: 'workspace', relativePath: '' },
                'Created images',
            ),
        );
    });

    it('constructs a persistent output reference in the selected writable directory', async () => {
        const onselect = vi.fn();
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'save-file',
                title: 'Select output',
                extensions: ['hds'],
                suggestedName: 'output.hds',
                onselect,
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        await fireEvent.click(await screen.findByText('images'));
        await waitFor(() =>
            expect((screen.getByLabelText('Output filename') as HTMLInputElement).value).toBe('output.hds'),
        );
        await fireEvent.click(screen.getByRole('button', { name: 'Select output' }));

        expect(onselect).toHaveBeenCalledWith({
            kind: 'server-file',
            reference: { rootId: 'workspace', relativePath: 'images/output.hds' },
            displayName: 'Yamaha images/images/output.hds',
        });
    });

    it('disables destination workflows for read-only storage locations', async () => {
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'directory',
                title: 'Choose image location',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        const readOnlyLocation = await screen.findByRole('button', { name: /Archive/ });
        expect(readOnlyLocation.hasAttribute('disabled')).toBe(true);
        expect(within(readOnlyLocation).getByText('Read-only location')).toBeTruthy();
    });

    it('offers storage-location management when no locations are configured', async () => {
        const onmanagelocations = vi.fn();
        render(ServerStoragePicker, {
            props: {
                transport: transport(false),
                mode: 'file',
                title: 'Open image',
                onmanagelocations,
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(await screen.findByText('No storage locations are configured.')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Manage storage locations' }));
        expect(onmanagelocations).toHaveBeenCalledOnce();
    });

    it('keeps primary and secondary actions aligned in the folder dialog', async () => {
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

        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'directory',
                title: 'Choose image location',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        await fireEvent.click(screen.getByRole('button', { name: 'New folder' }));
        const dialog = screen.getByRole('dialog', { name: 'Create folder' });
        const cancel = within(dialog).getByRole('button', { name: 'Cancel' });
        const create = within(dialog).getByRole('button', { name: 'Create' });
        const cancelStyle = getComputedStyle(cancel);
        const createStyle = getComputedStyle(create);

        expect(cancelStyle.height).toBe('30px');
        expect(createStyle.height).toBe('30px');
        expect(cancelStyle.marginTop).toBe('0px');
        expect(createStyle.marginTop).toBe('0px');
        expect(cancelStyle.marginBottom).toBe('0px');
        expect(createStyle.marginBottom).toBe('0px');

        style.remove();
    });
});
