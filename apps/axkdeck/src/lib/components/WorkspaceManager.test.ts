import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import workspaceManagerSource from './WorkspaceManager.svelte?raw';

const mocks = vi.hoisted(() => ({ invoke: vi.fn() }));
vi.mock('@tauri-apps/api/core', () => ({ invoke: mocks.invoke }));

import WorkspaceManager from './WorkspaceManager.svelte';

function workspaceResponse(): Response {
    return new Response(
        JSON.stringify({
            data: {
                state: 'NO_AVAILABLE_WORKSPACE',
                revision: 0,
                workspaces: [],
                configurationIssue: null,
            },
        }),
        { status: 200, headers: { 'content-type': 'application/json' } },
    );
}

function activeWorkspaceResponse(): Response {
    return new Response(
        JSON.stringify({
            data: {
                state: 'READY',
                revision: 2,
                workspaces: [
                    {
                        id: 'workspace-active',
                        displayName: 'Open image location',
                        path: '/tmp/open',
                        writable: true,
                        effectiveWritable: true,
                        status: 'AVAILABLE',
                        issue: null,
                    },
                    {
                        id: 'workspace-other',
                        displayName: 'Other location',
                        path: '/tmp/other',
                        writable: true,
                        effectiveWritable: true,
                        status: 'AVAILABLE',
                        issue: null,
                    },
                ],
                configurationIssue: null,
            },
        }),
        { status: 200, headers: { 'content-type': 'application/json' } },
    );
}

describe('WorkspaceManager', () => {
    beforeEach(() => {
        mocks.invoke.mockReset();
        window.__AXKLIB_SERVER__ = {
            baseUrl: 'http://127.0.0.1:7331/api/v1',
            bearerToken: 'test-token',
            mode: 'local',
        };
        vi.stubGlobal(
            'fetch',
            vi.fn().mockImplementation(() => Promise.resolve(workspaceResponse())),
        );
    });

    it('uses the shared compact dialog without a server storage eyebrow', async () => {
        render(WorkspaceManager, { props: { open: true, onclose: vi.fn() } });

        expect(
            (await screen.findByRole('dialog', { name: 'Storage locations' })).classList.contains('dialog-shell'),
        ).toBe(true);
        expect(screen.queryByText('Server storage')).toBeNull();
    });

    it('renders configured storage locations as a compact undivided list', async () => {
        vi.mocked(fetch).mockResolvedValue(activeWorkspaceResponse());
        render(WorkspaceManager, { props: { open: true, onclose: vi.fn() } });

        const list = await screen.findByRole('list', { name: 'Configured storage locations' });
        const rows = list.querySelectorAll<HTMLElement>('.workspace-row');
        expect(rows).toHaveLength(2);

        const listGeometry = workspaceManagerSource.match(/\.workspace-list\s*\{[^}]+\}/)?.[0];
        const rowGeometry = workspaceManagerSource.match(/\.workspace-row\s*\{[^}]+\}/)?.[0];
        const nameGeometry = workspaceManagerSource.match(/\.workspace-row strong\s*\{[^}]+\}/)?.[0];
        const statusGeometry = workspaceManagerSource.match(/\.workspace-row small\s*\{[^}]+\}/)?.[0];
        expect(listGeometry).toBeDefined();
        expect(rowGeometry).toBeDefined();
        expect(nameGeometry).toBeDefined();
        expect(statusGeometry).toBeDefined();
        const style = document.createElement('style');
        style.textContent = `${listGeometry}\n${rowGeometry}\n${nameGeometry}\n${statusGeometry}`;
        document.head.append(style);

        const listStyle = getComputedStyle(list);
        expect(listStyle.minHeight).toBe('auto');
        expect(listStyle.paddingTop).toBe('6px');
        expect(listStyle.paddingBottom).toBe('6px');
        expect(listStyle.gap).toBe('2px');

        const rowStyle = getComputedStyle(rows[0]);
        expect(rowStyle.minHeight).toBe('32px');
        expect(rowStyle.borderBottomStyle).toBe('none');

        const name = screen.getByText('Open image location');
        const status = rows[0].querySelector('small');
        expect(status).not.toBeNull();
        expect(getComputedStyle(name).lineHeight).toBe('11px');
        expect(getComputedStyle(status!).lineHeight).toBe('9px');
        expect(getComputedStyle(status!).marginTop).toBe('0px');

        style.remove();
    });

    it('opens the native picker and presents the selected directory for confirmation', async () => {
        mocks.invoke.mockResolvedValue({ candidateId: 'candidate-1', suggestedName: 'Samples' });
        render(WorkspaceManager, { props: { open: false, onclose: vi.fn() } });

        await fireEvent.click(await screen.findByRole('button', { name: 'Add storage location' }));

        expect(mocks.invoke).toHaveBeenCalledWith('select_local_workspace');
        await waitFor(() => expect((screen.getByLabelText('Location name') as HTMLInputElement).value).toBe('Samples'));
        const confirmation = screen.getByRole('dialog', { name: 'Add storage location' });
        expect(confirmation.closest('.dialog-backdrop')?.classList.contains('dialog-backdrop-raised')).toBe(true);
        expect(screen.getByRole('dialog', { name: 'Storage locations' })).toBeTruthy();
        expect(confirmation.querySelectorAll('button')).toHaveLength(3);
        expect(screen.queryByRole('list', { name: 'Configured storage locations' })).toBeNull();
    });

    it('shows that the operating-system picker is pending', async () => {
        let resolvePicker: (value: null) => void = () => undefined;
        mocks.invoke.mockImplementation(
            () =>
                new Promise<null>((resolve) => {
                    resolvePicker = resolve;
                }),
        );
        render(WorkspaceManager, { props: { open: false, onclose: vi.fn() } });

        await fireEvent.click(await screen.findByRole('button', { name: 'Add storage location' }));
        expect((screen.getByRole('button', { name: 'Choosing folder…' }) as HTMLButtonElement).disabled).toBe(true);
        resolvePicker(null);
        await waitFor(() =>
            expect((screen.getByRole('button', { name: 'Add storage location' }) as HTMLButtonElement).disabled).toBe(
                false,
            ),
        );
    });

    it('renders native picker failures instead of failing silently', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => undefined);
        mocks.invoke.mockRejectedValue(new Error('native dialog unavailable'));
        render(WorkspaceManager, { props: { open: false, onclose: vi.fn() } });

        await fireEvent.click(await screen.findByRole('button', { name: 'Add storage location' }));

        expect((await screen.findByRole('alert')).textContent).toContain('Native dialog unavailable');
    });

    it('protects only the storage location containing the open image', async () => {
        vi.mocked(fetch).mockResolvedValue(activeWorkspaceResponse());
        render(WorkspaceManager, {
            props: {
                open: true,
                activeWorkspaceId: 'workspace-active',
                onclose: vi.fn(),
            },
        });

        const activeRemove = await screen.findByRole('button', { name: 'Remove Open image location' });
        const otherRemove = screen.getByRole('button', { name: 'Remove Other location' });
        expect((activeRemove as HTMLButtonElement).disabled).toBe(true);
        expect(activeRemove.getAttribute('title')).toBe('Close the open image before removing this storage location');
        expect((otherRemove as HTMLButtonElement).disabled).toBe(false);
        expect((screen.getByRole('button', { name: 'Add storage location' }) as HTMLButtonElement).disabled).toBe(
            false,
        );
        expect(screen.getByText('In use by open image')).toBeTruthy();
    });
});
