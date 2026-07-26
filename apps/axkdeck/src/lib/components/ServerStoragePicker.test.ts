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
                ? [
                      {
                          name: 'nested.hds',
                          relativePath: 'images/nested.hds',
                          kind: 'FILE',
                          size: 2048,
                      },
                  ]
                : [
                      {
                          name: 'images',
                          relativePath: 'images',
                          kind: 'DIRECTORY',
                          size: null,
                      },
                      {
                          name: 'disk.hds',
                          relativePath: 'disk.hds',
                          kind: 'FILE',
                          size: 1024,
                      },
                      {
                          name: 'notes.txt',
                          relativePath: 'notes.txt',
                          kind: 'FILE',
                          size: 20,
                      },
                  ],
            truncated: false,
            nextCursor: null,
        })),
        inspectSandboxMediaSource: vi.fn().mockResolvedValue(null),
        createSandboxDirectory: vi.fn().mockResolvedValue(undefined),
    } as unknown as ImageTransport;
}

function activeOption(list: HTMLElement): HTMLElement {
    const activeId = list.getAttribute('aria-activedescendant');
    expect(activeId).toBeTruthy();
    const option = activeId ? document.getElementById(activeId) : null;
    expect(option).toBeTruthy();
    return option!;
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

    it('returns directly from a remembered directory to the workspace list', async () => {
        const ondirectorychange = vi.fn();
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Open image',
                initialDirectory: { rootId: 'workspace', relativePath: 'images' },
                ondirectorychange,
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(await screen.findByText('nested.hds')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Go to all workspaces' }));

        expect(await screen.findByText('Yamaha images')).toBeTruthy();
        expect(screen.getByText('Archive')).toBeTruthy();
        expect(screen.queryByText('nested.hds')).toBeNull();
        expect(screen.getByRole('button', { name: 'Go to all workspaces' }).hasAttribute('disabled')).toBe(true);
        expect(ondirectorychange).toHaveBeenLastCalledWith(null);
    });

    it('keeps parent navigation distinct from workspace home', async () => {
        const imageTransport = transport();
        const ondirectorychange = vi.fn();
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'file',
                title: 'Open image',
                initialDirectory: { rootId: 'workspace', relativePath: 'images/nested' },
                ondirectorychange,
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(await screen.findByText('Yamaha images/images/nested')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Parent directory' }));

        await waitFor(() =>
            expect(imageTransport.sandboxDirectory).toHaveBeenLastCalledWith({
                rootId: 'workspace',
                relativePath: 'images',
            }),
        );
        expect(screen.getByText('Yamaha images/images')).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Go to all workspaces' })).toBeTruthy();
        expect(ondirectorychange).toHaveBeenLastCalledWith({
            rootId: 'workspace',
            relativePath: 'images',
        });
    });

    it('keeps a persistent location bar with the path before the right-aligned home action', async () => {
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Open image',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        const location = screen.getByRole('navigation', { name: 'Storage location' });
        expect(within(location).getByText('Workspaces')).toBeTruthy();
        expect(within(location).getByRole('button', { name: 'Parent directory' }).hasAttribute('disabled')).toBe(true);
        expect(within(location).getByRole('button', { name: 'Go to all workspaces' }).hasAttribute('disabled')).toBe(
            true,
        );

        await fireEvent.click(await screen.findByText('Yamaha images'));

        const path = within(location).getByText('Yamaha images');
        const parent = within(location).getByRole('button', { name: 'Parent directory' });
        const home = within(location).getByRole('button', { name: 'Go to all workspaces' });
        expect(parent.hasAttribute('disabled')).toBe(false);
        expect(home.hasAttribute('disabled')).toBe(false);
        expect(path.compareDocumentPosition(home) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
        expect(location.lastElementChild?.contains(home)).toBe(true);
    });

    it('uses parent navigation to return from a workspace root to the workspace list', async () => {
        const ondirectorychange = vi.fn();
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'file',
                title: 'Open image',
                ondirectorychange,
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        expect(await screen.findByText('disk.hds')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Parent directory' }));

        expect(await screen.findByText('Archive')).toBeTruthy();
        expect(screen.queryByText('disk.hds')).toBeNull();
        expect(screen.getByRole('button', { name: 'Parent directory' }).hasAttribute('disabled')).toBe(true);
        expect(ondirectorychange).toHaveBeenLastCalledWith(null);
    });

    it('navigates roots, directories, and files with one keyboard selection cursor', async () => {
        const onselect = vi.fn();
        const imageTransport = transport();
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'file',
                title: 'Open image',
                extensions: ['hds'],
                onselect,
                oncancel: vi.fn(),
            },
        });

        await screen.findByText('Yamaha images');
        const list = screen.getByRole('listbox', { name: 'Storage entries' });
        await waitFor(() => expect(document.activeElement).toBe(list));
        expect(activeOption(list).textContent).toContain('Yamaha images');

        await fireEvent.keyDown(list, { key: 'ArrowRight' });
        await screen.findByText('disk.hds');
        expect(activeOption(list).textContent).toContain('images');

        await fireEvent.keyDown(list, { key: 'ArrowDown' });
        expect(activeOption(list).textContent).toContain('disk.hds');
        await fireEvent.keyDown(list, { key: 'Enter' });
        expect(onselect).toHaveBeenCalledWith({
            kind: 'server-file',
            reference: { rootId: 'workspace', relativePath: 'disk.hds' },
            displayName: 'Yamaha images/disk.hds',
        });

        await fireEvent.keyDown(list, { key: 'ArrowUp' });
        await fireEvent.keyDown(list, { key: 'ArrowRight' });
        await screen.findByText('nested.hds');
        expect(activeOption(list).textContent).toContain('nested.hds');

        await fireEvent.keyDown(list, { key: 'ArrowLeft' });
        await waitFor(() =>
            expect(imageTransport.sandboxDirectory).toHaveBeenLastCalledWith({
                rootId: 'workspace',
                relativePath: '',
            }),
        );
        expect(activeOption(list).textContent).toContain('images');

        const callsAtWorkspaceRoot = vi.mocked(imageTransport.sandboxDirectory).mock.calls.length;
        await fireEvent.keyDown(list, { key: 'ArrowLeft' });
        expect(vi.mocked(imageTransport.sandboxDirectory).mock.calls).toHaveLength(callsAtWorkspaceRoot);
        expect(await screen.findByText('Archive')).toBeTruthy();
        expect(within(list).getAllByRole('option')).toHaveLength(2);
        expect(screen.getByRole('button', { name: 'Parent directory' }).hasAttribute('disabled')).toBe(true);
    });

    it('skips disabled roots and supports first and last keyboard selection', async () => {
        render(ServerStoragePicker, {
            props: {
                transport: transport(),
                mode: 'directory',
                title: 'Choose image location',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await screen.findByText('Yamaha images');
        const list = screen.getByRole('listbox', { name: 'Storage entries' });
        expect(activeOption(list).textContent).toContain('Yamaha images');

        await fireEvent.keyDown(list, { key: 'End' });
        expect(activeOption(list).textContent).toContain('Yamaha images');
        await fireEvent.keyDown(list, { key: 'ArrowDown' });
        expect(activeOption(list).textContent).toContain('Yamaha images');
    });

    it('retains the current directory and selection when keyboard navigation fails', async () => {
        const imageTransport = transport();
        vi.mocked(imageTransport.sandboxDirectory).mockImplementation(async (directory) => {
            if (directory.relativePath === 'images') throw new Error('Directory became unavailable');
            return {
                directory,
                entries: [
                    {
                        name: 'images',
                        relativePath: 'images',
                        kind: 'DIRECTORY',
                        size: null,
                    },
                    {
                        name: 'disk.hds',
                        relativePath: 'disk.hds',
                        kind: 'FILE',
                        size: 1024,
                    },
                ],
                truncated: false,
                nextCursor: null,
            };
        });
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'file',
                title: 'Open image',
                extensions: ['hds'],
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        const list = screen.getByRole('listbox', { name: 'Storage entries' });
        expect(activeOption(list).textContent).toContain('images');
        await fireEvent.keyDown(list, { key: 'ArrowRight' });

        expect((await screen.findByRole('alert')).textContent).toContain('Directory became unavailable');
        expect(screen.getByTitle('Yamaha images')).toBeTruthy();
        expect(screen.getByText('disk.hds')).toBeTruthy();
        expect(activeOption(list).textContent).toContain('images');
    });

    it('does not treat arrow keys in the save filename as directory navigation', async () => {
        const imageTransport = transport();
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'save-file',
                title: 'Select output',
                extensions: ['hds'],
                suggestedName: 'output.hds',
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        const callsBeforeEditing = vi.mocked(imageTransport.sandboxDirectory).mock.calls.length;
        const filename = screen.getByLabelText('Output filename');
        await fireEvent.keyDown(filename, { key: 'ArrowLeft' });
        await fireEvent.keyDown(filename, { key: 'ArrowRight' });

        expect(vi.mocked(imageTransport.sandboxDirectory).mock.calls).toHaveLength(callsBeforeEditing);
        expect((filename as HTMLInputElement).value).toBe('output.hds');
    });

    it('selects the first matching entry loaded by a later page', async () => {
        const imageTransport = transport();
        vi.mocked(imageTransport.sandboxDirectory).mockImplementation(async (directory, cursor) => ({
            directory,
            entries: cursor
                ? [
                      {
                          name: 'disk.hds',
                          relativePath: 'disk.hds',
                          kind: 'FILE',
                          size: 1024,
                      },
                  ]
                : [
                      {
                          name: 'notes.txt',
                          relativePath: 'notes.txt',
                          kind: 'FILE',
                          size: 20,
                      },
                  ],
            truncated: !cursor,
            nextCursor: cursor ? null : 'next',
        }));
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'file',
                title: 'Open image',
                extensions: ['hds'],
                onselect: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        const list = screen.getByRole('listbox', { name: 'Storage entries' });
        expect(list.hasAttribute('aria-activedescendant')).toBe(false);
        await fireEvent.click(screen.getByRole('button', { name: 'Load more' }));

        expect(await screen.findByText('disk.hds')).toBeTruthy();
        expect(activeOption(list).textContent).toContain('disk.hds');
    });

    it('anchors storage pickers at a stable responsive top edge', () => {
        const backdropRule = appStyles.match(/\.storage-picker-backdrop\s*\{[^}]+\}/)?.[0];
        const pickerRule = appStyles.match(/\.storage-picker-backdrop \.storage-picker\s*\{[^}]+\}/)?.[0];
        const listRule = appStyles.match(/\.storage-picker-list\s*\{[^}]+\}/)?.[0];
        const disabledNavigationRule = appStyles.match(
            /\.storage-picker-location \.icon-button:disabled\s*\{[^}]+\}/,
        )?.[0];

        expect(backdropRule).toContain('place-items: start center');
        expect(backdropRule).toContain('clamp(16px, 6vh, 48px)');
        expect(pickerRule).toContain('height: min(720px,');
        expect(pickerRule).toContain('max-height:');
        expect(listRule).toContain('flex: 1 1 auto');
        expect(listRule).not.toContain('max-height: 222px');
        expect(disabledNavigationRule).toContain('cursor: default');
        expect(disabledNavigationRule).toContain('opacity: 0.35');
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

    it('selects several filtered files from one directory with pointer and keyboard controls', async () => {
        const imageTransport = transport();
        vi.mocked(imageTransport.sandboxDirectory).mockImplementation(async (directory) => ({
            directory,
            entries: [
                {
                    name: 'nested',
                    relativePath: 'nested',
                    kind: 'DIRECTORY',
                    size: null,
                },
                {
                    name: 'kick.wav',
                    relativePath: 'kick.wav',
                    kind: 'FILE',
                    size: 1024,
                },
                {
                    name: 'snare.FLAC',
                    relativePath: 'snare.FLAC',
                    kind: 'FILE',
                    size: 2048,
                },
                {
                    name: 'notes.txt',
                    relativePath: 'notes.txt',
                    kind: 'FILE',
                    size: 20,
                },
            ],
            truncated: false,
            nextCursor: null,
        }));
        const onselectmany = vi.fn();
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'file',
                multiple: true,
                title: 'Choose audio files',
                extensions: ['wav', 'wave', 'flac', 'aif', 'aiff'],
                onselect: vi.fn(),
                onselectmany,
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        expect(screen.queryByText('notes.txt')).toBeNull();
        const list = screen.getByRole('listbox', { name: 'Storage entries' });
        expect(list.getAttribute('aria-multiselectable')).toBe('true');

        const kick = screen.getByRole('option', { name: /kick\.wav/ });
        await fireEvent.click(kick);
        expect(kick.getAttribute('aria-selected')).toBe('true');

        await fireEvent.keyDown(list, { key: 'ArrowDown' });
        await fireEvent.keyDown(list, { key: 'ArrowDown' });
        expect(activeOption(list).textContent).toContain('snare.FLAC');
        await fireEvent.keyDown(list, { key: ' ' });

        await fireEvent.click(screen.getByRole('button', { name: 'Select 2 files' }));
        expect(onselectmany).toHaveBeenCalledWith([
            {
                kind: 'server-file',
                reference: { rootId: 'workspace', relativePath: 'kick.wav' },
                displayName: 'Yamaha images/kick.wav',
            },
            {
                kind: 'server-file',
                reference: { rootId: 'workspace', relativePath: 'snare.FLAC' },
                displayName: 'Yamaha images/snare.FLAC',
            },
        ]);
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

        const readOnlyLocation = await screen.findByRole('option', { name: /Archive/ });
        expect(readOnlyLocation.hasAttribute('disabled')).toBe(true);
        expect(within(readOnlyLocation).getByText('Read-only location')).toBeTruthy();
    });

    it('opens a recognized sampler object folder directly from its parent', async () => {
        const onselect = vi.fn();
        const imageTransport = transport();
        let finishInspection!: (kind: 'AXK_OBJECT_DIRECTORY' | null) => void;
        vi.mocked(imageTransport.inspectSandboxMediaSource).mockImplementation(
            () =>
                new Promise((resolve) => {
                    finishInspection = resolve;
                }),
        );
        vi.mocked(imageTransport.sandboxDirectory).mockResolvedValue({
            directory: { rootId: 'workspace', relativePath: '' },
            entries: [
                {
                    name: 'objects',
                    relativePath: 'objects',
                    kind: 'DIRECTORY',
                    size: null,
                },
                {
                    name: 'collection',
                    relativePath: 'collection',
                    kind: 'DIRECTORY',
                    size: null,
                },
                {
                    name: 'disk.hds',
                    relativePath: 'disk.hds',
                    kind: 'FILE',
                    size: 1024,
                },
            ],
            truncated: false,
            nextCursor: null,
        });
        render(ServerStoragePicker, {
            props: {
                transport: imageTransport,
                mode: 'media-source',
                title: 'Open image',
                extensions: ['hds'],
                onselect,
                oncancel: vi.fn(),
            },
        });

        await fireEvent.click(await screen.findByText('Yamaha images'));
        expect(screen.getByText('disk.hds')).toBeTruthy();
        expect(screen.getAllByText('Folder')).toHaveLength(2);
        expect(screen.getByText('Disk image · 1024 bytes')).toBeTruthy();
        expect(screen.queryByRole('button', { name: 'Open object directory' })).toBeNull();
        const objectDirectory = screen.getByRole('option', { name: /objects/ });
        const collection = screen.getByRole('option', { name: /collection/ });
        expect(objectDirectory.querySelector('[data-icon="chevron"]')).toBeTruthy();
        expect(collection.querySelector('[data-icon="chevron"]')).toBeTruthy();
        expect(imageTransport.inspectSandboxMediaSource).not.toHaveBeenCalled();
        await fireEvent.click(objectDirectory);

        expect(imageTransport.inspectSandboxMediaSource).toHaveBeenCalledWith({
            rootId: 'workspace',
            relativePath: 'objects',
        });
        expect(await screen.findByText('Inspecting')).toBeTruthy();
        finishInspection('AXK_OBJECT_DIRECTORY');
        await waitFor(() => expect(onselect).toHaveBeenCalled());
        expect(onselect).toHaveBeenCalledWith({
            kind: 'axk-object-directory',
            reference: { rootId: 'workspace', relativePath: 'objects' },
            displayName: 'Yamaha images/objects',
        });
        await fireEvent.click(screen.getByRole('option', { name: /disk.hds/ }));
        expect(onselect).toHaveBeenLastCalledWith({
            kind: 'server-file',
            reference: { rootId: 'workspace', relativePath: 'disk.hds' },
            displayName: 'Yamaha images/disk.hds',
        });
        expect(imageTransport.sandboxDirectory).toHaveBeenCalledWith({
            rootId: 'workspace',
            relativePath: '',
        });
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
