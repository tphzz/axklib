import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { AuditionWorkflow } from '../audition/workflow.svelte';
import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
import type { ImageTransport, JobState, ObjectDetail } from '../../lib/transport';
import type { ObjectParameterEdit } from '../../lib/objectEditing';
import SampleSaveHarness from '../../test/SampleSaveHarness.svelte';

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
                        current = {
                            ...current,
                            image: { ...current.image, revision },
                            editing: {
                                ...current.editing!,
                                payloadSha256: String(revision).repeat(64),
                                parameters: { ...current.editing!.parameters, ...submitted.operation.parameters },
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
                expect(view.getByText('Sample', { selector: 'strong' }).classList.contains('dirty')).toBe(true);
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
                expect(view.getByText('Sample', { selector: 'strong' }).classList.contains('dirty')).toBe(false);
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
