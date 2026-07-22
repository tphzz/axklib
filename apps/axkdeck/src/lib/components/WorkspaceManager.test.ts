import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';

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

        expect((await screen.findByRole('dialog', { name: 'Workspaces' })).classList.contains('dialog-shell')).toBe(
            true,
        );
        expect(screen.queryByText('Server storage')).toBeNull();
    });

    it('opens the native picker and presents the selected directory for confirmation', async () => {
        mocks.invoke.mockResolvedValue({ candidateId: 'candidate-1', suggestedName: 'Samples' });
        render(WorkspaceManager, { props: { open: false, onclose: vi.fn() } });

        await fireEvent.click(await screen.findByRole('button', { name: 'Add workspace' }));

        expect(mocks.invoke).toHaveBeenCalledWith('select_local_workspace');
        await waitFor(() =>
            expect((screen.getByLabelText('Workspace name') as HTMLInputElement).value).toBe('Samples'),
        );
        const confirmation = screen.getByRole('dialog', { name: 'Add workspace' });
        expect(confirmation.closest('.dialog-backdrop')?.classList.contains('dialog-backdrop-raised')).toBe(true);
        expect(screen.getByRole('dialog', { name: 'Workspaces' })).toBeTruthy();
        expect(confirmation.querySelectorAll('button')).toHaveLength(3);
        expect(screen.queryByRole('list', { name: 'Configured workspaces' })).toBeNull();
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

        await fireEvent.click(await screen.findByRole('button', { name: 'Add workspace' }));
        expect((screen.getByRole('button', { name: 'Choosing folder…' }) as HTMLButtonElement).disabled).toBe(true);
        resolvePicker(null);
        await waitFor(() =>
            expect((screen.getByRole('button', { name: 'Add workspace' }) as HTMLButtonElement).disabled).toBe(false),
        );
    });

    it('renders native picker failures instead of failing silently', async () => {
        vi.spyOn(console, 'error').mockImplementation(() => undefined);
        mocks.invoke.mockRejectedValue(new Error('native dialog unavailable'));
        render(WorkspaceManager, { props: { open: false, onclose: vi.fn() } });

        await fireEvent.click(await screen.findByRole('button', { name: 'Add workspace' }));

        expect((await screen.findByRole('alert')).textContent).toContain('Native dialog unavailable');
    });
});
