import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import { fireEvent, render, waitFor, within } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { AuditionWorkflow } from '../audition/workflow.svelte';
import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
import type { ImageTransport, JobState, ObjectDetail } from '../../lib/transport';
import { sampleSnapshot, type ObjectParameterEdit } from '../../lib/objectEditing';
import SampleSaveHarness from '../../test/SampleSaveHarness.svelte';
import { ObjectEditorWorkflow } from './workflow.svelte';

const native = vi.hoisted(() => ({
    invoke: vi.fn().mockResolvedValue(undefined),
    destroy: vi.fn().mockResolvedValue(undefined),
    close: null as null | ((event: { preventDefault: () => void }) => void),
    quit: null as null | (() => void),
}));
vi.mock('@tauri-apps/api/core', () => ({ invoke: native.invoke }));
vi.mock('@tauri-apps/api/window', () => ({
    getCurrentWindow: () => ({
        destroy: native.destroy,
        onCloseRequested: async (callback: typeof native.close) => {
            native.close = callback;
            return () => {
                native.close = null;
            };
        },
    }),
}));
vi.mock('@tauri-apps/api/event', () => ({
    listen: async (_: string, callback: typeof native.quit) => {
        native.quit = callback;
        return () => {
            native.quit = null;
        };
    },
}));

beforeEach(() => {
    vi.clearAllMocks();
    vi.stubGlobal('__TAURI_INTERNALS__', {});
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
});
afterEach(() => vi.unstubAllGlobals());

function setup() {
    let current = {
        image: { revision: 1 },
        object: { id: 'sample', key: 'sample', name: 'Sample' },
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: 'a'.repeat(64),
            parameters: {
                level: 100,
                pan: 0,
                coarse_tune: 0,
                loop_mode: 4,
                loop_start_frame: 0,
                loop_length_frames: 100,
            },
            playbackWindow: { start_frame: 0, length_frames: 100 },
            maximumFrames: 100,
            canEditPlayback: true,
            eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
            blockedParameters: [],
            blockedParameterReasons: {},
            ...sampleFormatFixture(),
            unavailableParameters: {},
            partitionIndex: 0,
            volumeName: 'Volume',
            sources: [],
        },
    } as unknown as ObjectDetail;
    let submitted: ObjectParameterEdit;
    let complete: () => void = () => {};
    const transport = {
        objectDetail: vi.fn(async () => structuredClone(current)),
        startObjectParameterEdit: vi.fn(async (_: number, edit: ObjectParameterEdit) => {
            submitted = edit;
            return { jobId: 7, status: 'queued' };
        }),
        waitForJob: vi.fn(
            () =>
                new Promise<JobState>((resolve) => {
                    complete = () => {
                        const revision = current.image.revision + 1;
                        const editing = sampleSnapshot(current)!;
                        current = {
                            ...current,
                            image: { ...current.image, revision },
                            editing: {
                                ...editing,
                                payloadSha256: String(revision).repeat(64),
                                parameters: { ...editing.parameters, ...submitted.operation.parameters },
                            },
                        };
                        resolve({ jobId: 7, status: 'completed' } as JobState);
                    };
                }),
        ),
    };
    const imageSession = {
        sessionId: 1,
        revision: 1,
        currentSourcePreference: vi.fn(),
        refresh: vi.fn().mockResolvedValue(undefined),
        setStatus: vi.fn(),
        confirmEditorLeave: async () => true,
    };
    const audition = {
        refreshEditorWorkspace: async (refresh: () => Promise<void>) => refresh(),
        stop: vi.fn().mockResolvedValue(undefined),
    };
    const view = render(SampleSaveHarness, {
        transport: transport as unknown as ImageTransport,
        imageSession: imageSession as unknown as ImageSessionWorkflow,
        audition: audition as unknown as AuditionWorkflow,
    });
    return { view, transport, imageSession, complete: () => complete() };
}

async function dirtyDraft() {
    const fixture = setup();
    await fireEvent.click(await fixture.view.findByRole('tab', { name: 'Map/Out' }));
    await fireEvent.click(fixture.view.getByRole('button', { name: 'Pitch' }));
    await fireEvent.input(fixture.view.getByRole('spinbutton', { name: 'Coarse tune' }), {
        target: { value: '7' },
    });
    return fixture;
}

describe('Unsaved editor exit confirmation', () => {
    it.each(['Discard', 'Save'] as const)(
        '%s keeps the mounted selection tracked when the structural action is cancelled',
        async (action) => {
            const { view, imageSession, transport, complete } = await dirtyDraft();
            const pending = imageSession.confirmEditorLeave();
            const dialog = await view.findByRole('dialog', { name: 'Unsaved edits' });
            await fireEvent.click(within(dialog).getByRole('button', { name: action }));
            if (action === 'Save') {
                await waitFor(() => expect(transport.waitForJob).toHaveBeenCalledOnce());
                complete();
            }
            expect(await pending).toBe(true);
            await waitFor(() => expect(view.queryByRole('dialog', { name: 'Unsaved edits' })).toBeNull());

            // Cancelling the subsequent structural dialog leaves the original selection mounted.
            const field = view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement;
            expect(field.value).toBe(action === 'Discard' ? '0' : '7');
            expect(view.getByRole('group', { name: 'Sample: Sample' })).toBeTruthy();
            expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(true);
            await fireEvent.input(field, { target: { value: '9' } });
            expect(view.getByRole('group', { name: 'Sample: Sample (unsaved changes)' })).toBeTruthy();
            expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(false);
            const next = imageSession.confirmEditorLeave();
            const nextDialog = await view.findByRole('dialog', { name: 'Unsaved edits' });
            expect(within(nextDialog).getByText(/1 unsaved draft/)).toBeTruthy();
            await fireEvent.click(within(nextDialog).getByRole('button', { name: 'Cancel' }));
            expect(await next).toBe(false);
            expect(field.value).toBe('9');
        },
    );

    it('keeps the draft and navigation pending if preparation throws', async () => {
        const { view, imageSession } = await dirtyDraft();
        const save = vi
            .spyOn(ObjectEditorWorkflow.prototype, 'saveAll')
            .mockRejectedValueOnce(new Error('Invalid draft'));
        const pending = imageSession.confirmEditorLeave();
        const dialog = await view.findByRole('dialog', { name: 'Unsaved edits' });
        await fireEvent.click(within(dialog).getByRole('button', { name: 'Save' }));
        expect(await within(dialog).findByText('Invalid draft')).toBeTruthy();
        expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(false);
        await fireEvent.click(within(dialog).getByRole('button', { name: 'Cancel' }));
        expect(await pending).toBe(false);
        save.mockRestore();
    });

    it.each(['Cancel', 'Escape', 'Discard'] as const)('%s resolves without starting a save', async (action) => {
        const { view, transport, imageSession } = await dirtyDraft();
        const pending = imageSession.confirmEditorLeave();
        const dialog = await view.findByRole('dialog', { name: 'Unsaved edits' });
        expect(within(dialog).getByText(/1 unsaved draft/)).toBeTruthy();
        expect(dialog.textContent).not.toContain('Sample');
        expect(
            within(dialog)
                .getAllByRole('button')
                .map((button) => button.textContent?.trim()),
        ).toEqual(['Cancel', 'Discard', 'Save']);
        if (action === 'Escape') await fireEvent.keyDown(dialog, { key: 'Escape' });
        else await fireEvent.click(within(dialog).getByRole('button', { name: action }));
        expect(await pending).toBe(action === 'Discard');
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
        expect(view.queryByRole('dialog', { name: 'Unsaved edits' })).toBeNull();
        if (action !== 'Discard') {
            expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('7');
            expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(false);
        } else {
            expect(await imageSession.confirmEditorLeave()).toBe(true);
            expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(true);
        }
    });

    it('Save continues only after the confirmed job and workspace refresh', async () => {
        const { view, transport, imageSession, complete } = await dirtyDraft();
        let refreshed!: () => void;
        imageSession.refresh.mockReturnValueOnce(new Promise<void>((resolve) => (refreshed = resolve)));
        const continued = vi.fn();
        const pending = imageSession.confirmEditorLeave().then(continued);
        const dialog = await view.findByRole('dialog', { name: 'Unsaved edits' });
        await fireEvent.click(within(dialog).getByRole('button', { name: 'Save' }));
        await waitFor(() => expect(transport.waitForJob).toHaveBeenCalledOnce());
        expect(continued).not.toHaveBeenCalled();
        for (const button of within(dialog).getAllByRole('button'))
            expect((button as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.keyDown(dialog, { key: 'Escape' });
        expect(continued).not.toHaveBeenCalled();
        complete();
        await waitFor(() => expect(imageSession.refresh).toHaveBeenCalledOnce());
        expect(continued).not.toHaveBeenCalled();
        expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(false);
        refreshed();
        await pending;
        expect(continued).toHaveBeenCalledExactlyOnceWith(true);
        await waitFor(() => expect(view.queryByRole('dialog', { name: 'Unsaved edits' })).toBeNull());
        expect(transport.startObjectParameterEdit).toHaveBeenCalledOnce();
        expect(await imageSession.confirmEditorLeave()).toBe(true);
        expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(true);
    });

    it.each(['failed', 'unconfirmed', 'refresh-failed'] as const)(
        'Save keeps navigation pending after a %s result',
        async (outcome) => {
            const { view, transport, imageSession, complete } = await dirtyDraft();
            if (outcome === 'failed')
                transport.waitForJob.mockResolvedValueOnce({
                    jobId: 7,
                    kind: 'alteration',
                    status: 'failed',
                    error: 'Rejected',
                });
            else if (outcome === 'unconfirmed')
                transport.waitForJob.mockRejectedValueOnce(new Error('Connection lost'));
            else imageSession.refresh.mockRejectedValueOnce(new Error('Refresh unavailable'));
            const continued = vi.fn();
            const pending = imageSession.confirmEditorLeave().then(continued);
            const dialog = await view.findByRole('dialog', { name: 'Unsaved edits' });
            await fireEvent.click(within(dialog).getByRole('button', { name: 'Save' }));
            await waitFor(() => expect(transport.waitForJob).toHaveBeenCalledOnce());
            if (outcome === 'refresh-failed') complete();
            await waitFor(() => expect(within(dialog).getByRole('status').textContent).toMatch(/did not complete/));
            expect(continued).not.toHaveBeenCalled();
            expect(transport.startObjectParameterEdit).toHaveBeenCalledOnce();
            expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(false);
            expect((within(dialog).getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(
                outcome !== 'failed',
            );
            expect((within(dialog).getByRole('button', { name: 'Discard' }) as HTMLButtonElement).disabled).toBe(
                outcome !== 'failed',
            );
            await fireEvent.click(within(dialog).getByRole('button', { name: 'Cancel' }));
            await pending;
            expect(continued).toHaveBeenCalledExactlyOnceWith(false);
            expect(view.queryByRole('dialog', { name: 'Unsaved edits' })).toBeNull();
            expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('7');
            expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(false);
        },
    );
});

describe('Sample save and exit lifecycle', () => {
    it.each(['close', 'quit'] as const)(
        'allows another edit and normal desktop %s after repeated saves',
        async (exit) => {
            const { view, transport, imageSession, complete } = setup();
            await fireEvent.click(await view.findByRole('tab', { name: 'Map/Out' }));
            await fireEvent.click(view.getByRole('button', { name: 'Pitch' }));
            await waitFor(() => expect(native.quit).not.toBeNull());

            for (const [index, value] of ['3', '7'].entries()) {
                let field = view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement;
                await fireEvent.input(field, { target: { value } });
                expect(view.getByRole('group', { name: 'Sample: Sample (unsaved changes)' })).toBeTruthy();
                await fireEvent.click(view.getByRole('button', { name: 'Save' }));
                await waitFor(() => expect(transport.waitForJob).toHaveBeenCalledTimes(index + 1));
                expect(field.disabled).toBe(true);
                expect(await imageSession.confirmEditorLeave()).toBe(false);
                expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(false);
                complete();
                await waitFor(() => {
                    field = view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement;
                    expect(field.disabled).toBe(false);
                });
                expect(field.value).toBe(value);
                expect(view.getByRole('group', { name: 'Sample: Sample' })).toBeTruthy();
                expect(view.queryByRole('button', { name: 'Check status' })).toBeNull();
                expect((view.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(true);
                expect(view.getByRole('tab', { name: 'Map/Out' }).getAttribute('aria-selected')).toBe('true');
                expect(view.getByRole('button', { name: 'Pitch' }).getAttribute('aria-pressed')).toBe('true');
                expect(await imageSession.confirmEditorLeave()).toBe(true);
                expect(window.dispatchEvent(new Event('beforeunload', { cancelable: true }))).toBe(true);
                await waitFor(() =>
                    expect(native.invoke).toHaveBeenLastCalledWith('set_editor_exit_guard', { blocked: false }),
                );
            }
            expect(transport.startObjectParameterEdit.mock.calls.map(([, edit]) => edit.expectedRevision)).toEqual([
                1, 2,
            ]);
            expect(transport.startObjectParameterEdit.mock.calls[1]![1].operation.parameters).toEqual({
                coarse_tune: 7,
            });
            if (exit === 'close') {
                native.close!({ preventDefault: vi.fn() });
                await waitFor(() => expect(native.destroy).toHaveBeenCalledOnce());
            } else {
                native.quit!();
                await waitFor(() => expect(native.invoke).toHaveBeenCalledWith('approve_editor_exit'));
            }
        },
    );
});
