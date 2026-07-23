/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { ImageTransport } from '../transport';
import ServerStoragePicker from './ServerStoragePicker.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');

function transport(): ImageTransport {
    return {
        storageMode: 'server',
        sandboxRoots: vi.fn().mockResolvedValue([
            { id: 'workspace', displayName: 'Workspace', writable: true },
            { id: 'readonly', displayName: 'Archive', writable: false },
        ]),
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
        renameSandboxEntry: vi.fn().mockResolvedValue(undefined),
        deleteSandboxEntry: vi.fn().mockResolvedValue(undefined),
    } as unknown as ImageTransport;
}

describe('ServerStoragePicker', () => {
    it('uses the compact user-facing dialog title without a server filesystem eyebrow', async () => {
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Open disk image',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('dialog', { name: 'Open disk image' }).classList.contains('dialog-shell')).toBe(true);
        expect(screen.queryByText('Server filesystem')).toBeNull();
        expect(await screen.findByText('Workspace')).toBeTruthy();
    });

    it('restores and reports the session directory supplied by its owner', async () => {
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

    it('returns only an exact sandbox FileRef and filters unrelated files', async () => {
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

        await fireEvent.click(await screen.findByText('Workspace'));
        expect(screen.queryByText('notes.txt')).toBeNull();
        await fireEvent.click(await screen.findByText('disk.hds'));

        expect(onselect).toHaveBeenCalledWith({
            kind: 'server-file',
            reference: { rootId: 'workspace', relativePath: 'disk.hds' },
            displayName: 'Workspace/disk.hds',
        });
    });

    it('keeps file rows free of misleading media icons and identifies the current path', async () => {
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

        await fireEvent.click(await screen.findByText('Workspace'));

        const fileButton = screen.getByText('disk.hds').closest('button');
        if (!fileButton) throw new Error('disk image row button is missing');
        expect(fileButton.classList.contains('storage-picker-file-row')).toBe(true);
        expect(fileButton.querySelector('svg')).toBeNull();
        expect(screen.getByText('Workspace').classList.contains('storage-picker-path')).toBe(true);
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

        await fireEvent.click(await screen.findByText('Workspace'));
        await fireEvent.click(await screen.findByText('images'));
        await waitFor(() =>
            expect((screen.getByLabelText('Output filename') as HTMLInputElement).value).toBe('output.hds'),
        );
        await fireEvent.click(screen.getByRole('button', { name: 'Select output' }));

        expect(onselect).toHaveBeenCalledWith({
            kind: 'server-file',
            reference: { rootId: 'workspace', relativePath: 'images/output.hds' },
            displayName: 'Workspace/images/output.hds',
        });
    });

    it('creates, renames, and permanently deletes entries in writable roots', async () => {
        const imageTransport = transport();
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'file',
                title: 'Open image',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Workspace'));
        await fireEvent.click(screen.getByRole('button', { name: 'New folder' }));
        await fireEvent.input(screen.getByLabelText('Folder name'), { target: { value: 'Imports' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Create' }));
        await waitFor(() =>
            expect(imageTransport.createSandboxDirectory).toHaveBeenCalledWith(
                { rootId: 'workspace', relativePath: '' },
                'Imports',
            ),
        );

        await fireEvent.click(screen.getByRole('button', { name: 'More actions for disk.hds' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Rename' }));
        const renameInput = screen.getByLabelText('Entry name') as HTMLInputElement;
        expect(renameInput.value).toBe('disk.hds');
        await fireEvent.input(renameInput, { target: { value: 'renamed.hds' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Rename entry' }));
        await waitFor(() =>
            expect(imageTransport.renameSandboxEntry).toHaveBeenCalledWith(
                { rootId: 'workspace', relativePath: 'disk.hds' },
                'renamed.hds',
            ),
        );

        await fireEvent.click(screen.getByRole('button', { name: 'More actions for disk.hds' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
        expect(screen.getByText('This action cannot be undone.')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Delete permanently' }));
        await waitFor(() =>
            expect(imageTransport.deleteSandboxEntry).toHaveBeenCalledWith({
                rootId: 'workspace',
                relativePath: 'disk.hds',
            }),
        );
    });

    it('runs owner cleanup before deleting a selected file', async () => {
        const imageTransport = transport();
        const onbeforedelete = vi.fn().mockResolvedValue(undefined);
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'file',
                title: 'Open image',
                onbeforedelete,
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Workspace'));
        await fireEvent.click(screen.getByRole('button', { name: 'More actions for disk.hds' }));
        await fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Delete permanently' }));

        await waitFor(() =>
            expect(onbeforedelete).toHaveBeenCalledWith({ rootId: 'workspace', relativePath: 'disk.hds' }),
        );
        expect(onbeforedelete.mock.invocationCallOrder[0]).toBeLessThan(
            vi.mocked(imageTransport.deleteSandboxEntry).mock.invocationCallOrder[0],
        );
    });

    it('keeps primary and secondary actions vertically aligned in nested dialogs', async () => {
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
                mode: 'file',
                title: 'Open image',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Workspace'));
        await fireEvent.click(screen.getByRole('button', { name: 'New folder' }));
        const dialog = screen.getByRole('dialog', { name: 'Create folder' });
        const cancel = within(dialog).getByRole('button', { name: 'Cancel' });
        const create = within(dialog).getByRole('button', { name: 'Create' });
        const cancelStyle = getComputedStyle(cancel);
        const createStyle = getComputedStyle(create);

        expect(cancelStyle.display).toBe('inline-flex');
        expect(createStyle.display).toBe('inline-flex');
        expect(cancelStyle.alignItems).toBe('center');
        expect(createStyle.alignItems).toBe('center');
        expect(cancelStyle.height).toBe('30px');
        expect(createStyle.height).toBe('30px');
        expect(cancelStyle.marginTop).toBe('0px');
        expect(createStyle.marginTop).toBe('0px');
        expect(cancelStyle.marginBottom).toBe('0px');
        expect(createStyle.marginBottom).toBe('0px');

        style.remove();
    });

    it('renders entry actions outside the scrollable directory list', async () => {
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Open image',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Workspace'));
        await fireEvent.click(screen.getByRole('button', { name: 'More actions for disk.hds' }));

        const menu = screen.getByRole('menu');
        expect(menu.closest('.storage-picker-list')).toBeNull();
        expect(menu.closest('.storage-picker')).toBeNull();
    });

    it('does not expose file management controls in read-only roots', async () => {
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Open image',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Archive'));
        expect(screen.queryByRole('button', { name: 'New folder' })).toBeNull();
        expect(screen.queryByRole('button', { name: /More actions/ })).toBeNull();
    });

    it('offers a generic writable-directory action with the exact current directory', async () => {
        const onactivate = vi.fn();
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Disk images',
                directoryAction: { label: 'New HD image', icon: 'hard-drive', onactivate },
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Workspace'));
        const newFolder = screen.getByRole('button', { name: 'New folder' });
        const newDisk = screen.getByRole('button', { name: 'New HD image' });
        expect(newDisk.compareDocumentPosition(newFolder) & Node.DOCUMENT_POSITION_FOLLOWING).not.toBe(0);
        expect(newFolder.classList.contains('storage-picker-directory-action')).toBe(true);
        expect(newDisk.classList.contains('storage-picker-directory-action')).toBe(true);

        const actionGeometry = appStyles.match(/\.storage-picker-directory-action\s*\{[^}]+\}/)?.[0];
        expect(actionGeometry).toBeDefined();
        const style = document.createElement('style');
        style.textContent = actionGeometry ?? '';
        document.head.append(style);
        expect(getComputedStyle(newFolder).width).toBe('148px');
        expect(getComputedStyle(newDisk).width).toBe('148px');
        expect(getComputedStyle(newFolder).height).toBe('26px');
        expect(getComputedStyle(newDisk).height).toBe('26px');
        style.remove();

        await fireEvent.click(newDisk);
        expect(onactivate).toHaveBeenCalledWith({
            kind: 'server-directory',
            reference: { rootId: 'workspace', relativePath: '' },
            displayName: 'Workspace',
        });
    });
});
