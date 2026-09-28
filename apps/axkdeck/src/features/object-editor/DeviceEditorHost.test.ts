import { sampleConversionFixture, sampleFormatFixture } from '../../test/sampleFormatFixture';
import { act, fireEvent, render, waitFor, within } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import type { SampleWaveformPreview } from '../../lib/types';
import DeviceEditorHostHarness from '../../test/DeviceEditorHostHarness.svelte';
import { ObjectEditorWorkflow } from './workflow.svelte';
import type { EditorAudioServices } from './audioContext';

beforeEach(() => vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null));

function detail(id: string): ObjectDetail {
    return {
        image: { revision: 1 },
        object: { id, key: id, name: `Sample ${id}` },
        formatConversion: sampleConversionFixture(),
        editing: {
            profile: 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: 'a'.repeat(64),
            parameters: {
                level: 100,
                pan: 0,
                coarse_tune: id === 'A' ? 3 : -2,
                fine_tune_cents: 0,
                fixed_pitch: false,
                random_pitch: 0,
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
}

function deferred<T>() {
    let resolve!: (value: T) => void;
    let reject!: (reason: Error) => void;
    const promise = new Promise<T>((yes, no) => {
        resolve = yes;
        reject = no;
    });
    return { promise, resolve, reject };
}

function preview(id: string): SampleWaveformPreview {
    return {
        previewState: 'ready',
        preview: {
            lanes: ['left', 'right'].map((role) => ({
                role: `${id} ${role}`,
                sampleRate: 44100,
                bins: [{ minimum: -0.5, maximum: 0.5 }],
            })),
        },
    } as unknown as SampleWaveformPreview;
}

function setup(initialPreview?: SampleWaveformPreview, audio?: EditorAudioServices) {
    const transport = {
        objectDetail: vi.fn(async (_: number, id: string) => detail(id)),
        startObjectParameterEdit: vi.fn().mockResolvedValue({ jobId: 7, status: 'queued' }),
        waitForJob: vi.fn().mockResolvedValue({ jobId: 7, status: 'completed' }),
    };
    const workflow = new ObjectEditorWorkflow({
        transport,
        refresh: vi.fn().mockResolvedValue(undefined),
        stopPlayback: vi.fn(),
        status: vi.fn(),
    });
    return { workflow, transport, view: render(DeviceEditorHostHarness, { workflow, preview: initialPreview, audio }) };
}

describe('Sample editor workspace navigation', () => {
    it('keeps identity accessible and conversion available without repeating the format badge', async () => {
        const { view, workflow } = setup();
        await view.findByRole('tab', { name: 'Trim/Loop' });
        const header = view.container.querySelector('.device-editor header')!;
        expect(header.textContent).not.toContain('A-series');
        expect(header.textContent).not.toContain('Sample A');
        expect(view.getByRole('group', { name: 'Sample: Sample A' })).toBeTruthy();
        const actions = header.querySelector('.actions')!;
        expect(header.querySelector('.format-badge')).toBeNull();
        expect(within(actions as HTMLElement).getByRole('button', { name: /Convert to a3k/ })).toBeTruthy();

        await act(() => workflow.find(1, 'A')!.draft.set('level', 99));
        expect(view.getByRole('group', { name: 'Sample: Sample A (unsaved changes)' })).toBeTruthy();
        expect(header.textContent).not.toContain('Sample A');
        expect((view.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(view.getByRole('button', { name: 'Discard' }));
        await view.findByRole('group', { name: 'Sample: Sample A' });
    });

    it.each(['A3000_188', 'A4000_A5000_224'] as const)(
        'retains Bank identity, format controls and recovery actions without a visible name (%s)',
        async (format) => {
            const { view, transport, workflow } = setup();
            await view.findByRole('tab', { name: 'Trim/Loop' });
            const bank = detail('bank');
            bank.object.name = 'Long Bank Name';
            bank.object.type = 'SBAC';
            bank.editing!.profile = 'a-series/sample-bank';
            bank.editing!.bankOverrides = {
                units: [{ id: 33, keys: ['level'], selectors: [33], activeSelectors: [33] }],
                members: [],
            };
            Object.assign(bank.editing!, sampleFormatFixture(format));
            bank.formatConversion = sampleConversionFixture(format);
            transport.objectDetail.mockResolvedValue(bank);
            await view.rerender({ sample: 'bank', kind: 'sample-bank' });
            await view.findByRole('group', { name: 'Sample Bank: Long Bank Name' });
            const header = view.container.querySelector('.device-editor header')!;
            expect(header.textContent).not.toContain('Long Bank Name');
            expect(header.querySelector('.format-badge')).toBeNull();
            await fireEvent.click(view.getByRole('button', { name: /Convert to/ }));
            expect(workflow.conversionDocument).toBe(workflow.find(1, 'bank'));
            workflow.conversionDocument = null;
            const document = workflow.find(1, 'bank')!;
            await act(() => document.draft.set('level', 99));
            expect(view.getByRole('group', { name: 'Sample Bank: Long Bank Name (unsaved changes)' })).toBeTruthy();
            const recover = vi.spyOn(workflow, 'recover').mockResolvedValue(undefined);
            await act(() => {
                document.phase = 'refresh-failed';
            });
            await fireEvent.click(view.getByRole('button', { name: 'Refresh' }));
            expect(recover).toHaveBeenCalledWith(document);
            await act(() => {
                document.phase = 'unconfirmed';
                document.jobId = 7;
            });
            await fireEvent.click(view.getByRole('button', { name: 'Check status' }));
            expect(recover).toHaveBeenCalledTimes(2);
        },
    );

    it('retains the outgoing editor and waveform inertly until the next document and preview can swap together', async () => {
        const { view, transport, workflow } = setup(preview('A'));
        await view.findByRole('group', { name: /^Sample: Sample A(?: \(unsaved changes\))?$/ });
        const panel = view.getByRole('tabpanel');
        const waveform = view.getByRole('region', { name: 'Waveform editor' });
        const pending = deferred<ObjectDetail>();
        transport.objectDetail.mockImplementationOnce(() => pending.promise);

        await view.rerender({ sample: 'B', preview: preview('B') });

        expect(view.container.querySelector('.editor-content[aria-label^="Sample: Sample A"]')).toBeTruthy();
        expect(panel.isConnected).toBe(true);
        expect(waveform.isConnected).toBe(true);
        expect(view.getByText('A left')).toBeTruthy();
        expect(view.queryByText('B left')).toBeNull();
        expect(view.container.querySelector('.editor-content[aria-label^="Sample: Sample B"]')).toBeNull();
        expect(panel.closest('[inert]')).not.toBeNull();
        expect(view.getByRole('region', { name: 'Sample editor' }).getAttribute('aria-busy')).toBe('true');
        const status = view.getAllByRole('status').find((element) => !element.closest('[inert]'));
        expect(status?.textContent).toMatch(/loading/i);
        expect(workflow.visible).toBe(true);

        pending.resolve(detail('B'));
        await view.findByRole('group', { name: /^Sample: Sample B(?: \(unsaved changes\))?$/ });
        expect(view.container.querySelector('.editor-content[aria-label^="Sample: Sample A"]')).toBeNull();
        expect(view.queryByText('A left')).toBeNull();
        expect(view.getByText('B left')).toBeTruthy();
        expect(view.getByRole('tabpanel').closest('[inert]')).toBeNull();
        expect(view.getByRole('region', { name: 'Sample editor' }).getAttribute('aria-busy')).not.toBe('true');
    });

    it('keeps the active tab, subpage, and outgoing controls mounted while a replacement loads', async () => {
        const { view, transport } = setup();
        await fireEvent.click(await view.findByRole('tab', { name: 'Map/Out' }));
        await fireEvent.click(view.getByRole('button', { name: 'Pitch' }));
        const panel = view.getByRole('tabpanel');
        const input = view.getByRole('spinbutton', { name: 'Coarse tune' });
        const pending = deferred<ObjectDetail>();
        transport.objectDetail.mockImplementationOnce(() => pending.promise);

        await view.rerender({ sample: 'B' });
        expect(input.isConnected).toBe(true);
        expect(panel.isConnected).toBe(true);
        expect(input.closest('[inert]')).not.toBeNull();
        pending.resolve(detail('B'));

        await view.findByRole('group', { name: /^Sample: Sample B(?: \(unsaved changes\))?$/ });
        expect(view.getByRole('tab', { name: 'Map/Out' }).getAttribute('aria-selected')).toBe('true');
        expect(view.getByRole('button', { name: 'Pitch' }).getAttribute('aria-pressed')).toBe('true');
        expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('-2');
    });

    it('blocks the outgoing transport window shortcut while a replacement is loading or has failed', async () => {
        const playPrepared = vi.fn().mockResolvedValue(undefined);
        const audio: EditorAudioServices = {
            transport: {} as EditorAudioServices['transport'],
            audition: {
                state: { objectId: null, status: 'idle', playheadFrame: 0 },
                autoplay: false,
                playPrepared,
                seekPrepared: vi.fn(),
                stop: vi.fn().mockResolvedValue(undefined),
            },
        };
        const { view, transport } = setup(undefined, audio);
        await view.findByRole('group', { name: /^Sample: Sample A(?: \(unsaved changes\))?$/ });
        await fireEvent.keyDown(document.body, { key: ' ', code: 'Space' });
        expect(playPrepared).toHaveBeenCalledOnce();
        playPrepared.mockClear();
        const pending = deferred<ObjectDetail>();
        transport.objectDetail.mockImplementationOnce(() => pending.promise);
        await view.rerender({ sample: 'B' });

        await fireEvent.keyDown(document.body, { key: ' ', code: 'Space' });
        expect(playPrepared).not.toHaveBeenCalled();
        pending.reject(new Error('Selected Sample unavailable'));
        await view.findByText('Selected Sample unavailable');
        await fireEvent.keyDown(document.body, { key: ' ', code: 'Space' });
        expect(playPrepared).not.toHaveBeenCalled();
    });

    it('waits for a pending waveform after parameters resolve and accepts the latest matching preview atomically', async () => {
        const { view, workflow } = setup(preview('A'));
        await view.findByRole('group', { name: /^Sample: Sample A(?: \(unsaved changes\))?$/ });
        const panel = view.getByRole('tabpanel');
        await view.rerender({
            sample: 'B',
            preview: { ...preview('B'), previewState: 'loading', preview: null },
        });
        await waitFor(() => expect(workflow.find(1, 'B')).toBeDefined());

        expect(view.container.querySelector('.editor-content[aria-label^="Sample: Sample A"]')).toBeTruthy();
        expect(view.getByText('A left')).toBeTruthy();
        expect(panel.isConnected).toBe(true);
        expect(panel.closest('[inert]')).not.toBeNull();
        await view.rerender({ preview: preview('B ready') });

        await view.findByRole('group', { name: /^Sample: Sample B(?: \(unsaved changes\))?$/ });
        expect(view.getByText('B ready left')).toBeTruthy();
        expect(view.queryByText('A left')).toBeNull();
        expect(view.getByRole('tabpanel').closest('[inert]')).toBeNull();
    });

    it.each(['resolve', 'reject'] as const)('ignores a stale %s after a newer selection finishes', async (outcome) => {
        const { view, transport } = setup(preview('A'));
        await view.findByRole('group', { name: /^Sample: Sample A(?: \(unsaved changes\))?$/ });
        const stale = deferred<ObjectDetail>();
        const current = deferred<ObjectDetail>();
        transport.objectDetail
            .mockImplementationOnce(() => stale.promise)
            .mockImplementationOnce(() => current.promise);

        await view.rerender({ sample: 'B', preview: preview('B') });
        await view.rerender({ sample: 'C', preview: preview('C') });
        current.resolve(detail('C'));
        await view.findByRole('group', { name: /^Sample: Sample C(?: \(unsaved changes\))?$/ });
        await act(async () => {
            if (outcome === 'resolve') stale.resolve(detail('B'));
            else stale.reject(new Error('Stale Sample B failure'));
            await stale.promise.catch(() => undefined);
        });
        await waitFor(() => expect(transport.objectDetail).toHaveBeenCalledTimes(3));

        expect(view.container.querySelector('.editor-content[aria-label^="Sample: Sample C"]')).toBeTruthy();
        expect(view.getByText('C left')).toBeTruthy();
        expect(view.queryByText('B left')).toBeNull();
        expect(view.container.querySelector('.editor-content[aria-label^="Sample: Sample B"]')).toBeNull();
        expect(view.queryByText('Stale Sample B failure')).toBeNull();
    });

    it('reports a failed replacement while preserving the outgoing editor as non-interactive context', async () => {
        const { view, transport } = setup(preview('A'));
        await view.findByRole('group', { name: /^Sample: Sample A(?: \(unsaved changes\))?$/ });
        const panel = view.getByRole('tabpanel');
        const pending = deferred<ObjectDetail>();
        transport.objectDetail.mockImplementationOnce(() => pending.promise);
        await view.rerender({ sample: 'B', preview: preview('B') });
        pending.reject(new Error('Sample B parameters could not be loaded'));

        const error = await view.findByText('Sample B parameters could not be loaded');
        expect(error.closest('[role="status"], [role="alert"]')).not.toBeNull();
        expect(error.closest('[inert]')).toBeNull();
        expect(view.container.querySelector('.editor-content[aria-label^="Sample: Sample A"]')).toBeTruthy();
        expect(view.getByText('A left')).toBeTruthy();
        expect(view.queryByText('B left')).toBeNull();
        expect(panel.isConnected).toBe(true);
        expect(panel.closest('[inert]')).not.toBeNull();
        expect(view.getByRole('region', { name: 'Sample editor' }).getAttribute('aria-busy')).not.toBe('true');
    });

    it('does not mount parameter controls for a conversion-only Sample with an unknown layout', async () => {
        const unsupported = detail('unknown');
        unsupported.editing = null;
        unsupported.formatConversion = sampleConversionFixture('UNKNOWN', {
            canConvertFormat: false,
            reason: 'Unknown stored format',
        });
        const transport = {
            objectDetail: vi.fn().mockResolvedValue(unsupported),
            startObjectParameterEdit: vi.fn(),
            waitForJob: vi.fn(),
        };
        const workflow = new ObjectEditorWorkflow({
            transport,
            refresh: vi.fn(),
            stopPlayback: vi.fn(),
            status: vi.fn(),
        });
        const view = render(DeviceEditorHostHarness, { workflow, sample: 'unknown' });
        expect(await view.findByText('Editing is not available for this Sample format')).toBeTruthy();
        expect(view.queryByRole('tab')).toBeNull();
        expect(workflow.visible).toBe(false);
        await workflow.openConversion(1, 'unknown');
        expect(workflow.conversionDocument?.detail?.formatConversion?.canConvertFormat).toBe(false);
    });
    it('does not remount the editor when the active document receives refreshed metadata', async () => {
        const { view, workflow } = setup();
        await fireEvent.click(await view.findByRole('tab', { name: 'Map/Out' }));
        await fireEvent.click(view.getByRole('button', { name: 'Pitch' }));
        const panel = view.getByRole('tabpanel');
        const input = view.getByRole('spinbutton', { name: 'Coarse tune' });
        panel.scrollTop = 73;
        await workflow.check(workflow.find(1, 'A')!);
        await waitFor(() => expect(view.getByRole('spinbutton', { name: 'Coarse tune' })).toBe(input));
        expect(view.getByRole('tabpanel')).toBe(panel);
        expect(panel.scrollTop).toBe(73);
    });

    it('keeps the active tab and subpage when switching Samples without sharing their drafts or undo histories', async () => {
        const { view, transport } = setup();
        await fireEvent.click(await view.findByRole('tab', { name: 'Map/Out' }));
        await fireEvent.click(view.getByRole('button', { name: 'Pitch' }));
        await fireEvent.input(view.getByRole('spinbutton', { name: 'Coarse tune' }), {
            target: { value: '12' },
        });

        await view.rerender({ sample: 'B' });
        await view.findByRole('group', { name: /^Sample: Sample B(?: \(unsaved changes\))?$/ });
        expect(view.getByRole('tab', { name: 'Map/Out' }).getAttribute('aria-selected')).toBe('true');
        expect(view.getByRole('button', { name: 'Pitch' }).getAttribute('aria-pressed')).toBe('true');
        const tuneB = view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement;
        expect(tuneB.value).toBe('-2');
        expect((view.getByRole('button', { name: 'Undo Sample edit' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.input(tuneB, { target: { value: '-10' } });

        await view.rerender({ sample: 'A' });
        await view.findByRole('group', { name: /^Sample: Sample A(?: \(unsaved changes\))?$/ });
        expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('12');
        await fireEvent.click(view.getByRole('button', { name: 'Undo Sample edit' }));
        expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('3');

        await view.rerender({ sample: 'B' });
        await view.findByRole('group', { name: /^Sample: Sample B(?: \(unsaved changes\))?$/ });
        expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('-10');
        await fireEvent.click(view.getByRole('button', { name: 'Discard' }));
        await waitFor(() =>
            expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('-2'),
        );
        expect(view.getByRole('tab', { name: 'Map/Out' }).getAttribute('aria-selected')).toBe('true');
        expect(view.getByRole('button', { name: 'Pitch' }).getAttribute('aria-pressed')).toBe('true');
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
    });

    it('retains keyboard-selected navigation after hiding the editor and selecting another Sample', async () => {
        const { view } = setup();
        const trimLoop = await view.findByRole('tab', { name: 'Trim/Loop' });
        await fireEvent.keyDown(trimLoop, { key: 'ArrowRight' });
        expect(view.getByRole('tab', { name: 'Map/Out' }).getAttribute('aria-selected')).toBe('true');
        const pages = within(view.getByRole('navigation', { name: 'Sample subpages' }));
        await fireEvent.keyDown(pages.getByRole('button', { name: 'Mix & Key' }), { key: 'ArrowRight' });
        expect(pages.getByRole('button', { name: 'Pitch' }).getAttribute('aria-pressed')).toBe('true');

        await view.rerender({ visible: false });
        expect(view.queryByRole('region', { name: 'Sample editor' })).toBeNull();
        await view.rerender({ visible: true, sample: 'B' });
        await view.findByRole('group', { name: /^Sample: Sample B(?: \(unsaved changes\))?$/ });
        expect(view.getByRole('tab', { name: 'Map/Out' }).getAttribute('aria-selected')).toBe('true');
        expect(view.getByRole('button', { name: 'Pitch' }).getAttribute('aria-pressed')).toBe('true');
    });

    it('resets workspace navigation when the image session is cleared', async () => {
        const { view, workflow } = setup();
        await fireEvent.click(await view.findByRole('tab', { name: 'Map/Out' }));
        await fireEvent.click(view.getByRole('button', { name: 'Pitch' }));
        await view.rerender({ visible: false });
        workflow.clear();
        await view.rerender({ visible: true, sessionId: 2, sample: 'B' });
        await view.findByRole('group', { name: /^Sample: Sample B(?: \(unsaved changes\))?$/ });
        expect(view.getByRole('tab', { name: 'Trim/Loop' }).getAttribute('aria-selected')).toBe('true');
        expect(view.getByRole('button', { name: 'Waveform' }).getAttribute('aria-pressed')).toBe('true');
    });

    it('updates difference markers and named-value tooltips while editing only the active Sample', async () => {
        const { view, workflow, transport } = setup();
        await fireEvent.click(await view.findByRole('tab', { name: 'Map/Out' }));
        await fireEvent.click(view.getByRole('button', { name: 'Pitch' }));
        await workflow.comparison.select(1, 1, ['A', 'B']);

        const label = view.container.querySelector('[data-parameter="coarse_tune"]')!;
        await waitFor(() => expect(label.getAttribute('data-different')).toBe('true'));
        expect(label.querySelector('[aria-label="Different values"]')).not.toBeNull();
        expect(view.getByText('Comparing 2 Samples').getAttribute('title')).toBe('Editing Sample A only');
        expect(view.container.querySelector('[data-parameter="fine_tune_cents"]')?.hasAttribute('data-different')).toBe(
            false,
        );
        await fireEvent.focus(view.getByRole('button', { name: 'Coarse tune' }));
        const tooltip = view.getByRole('tooltip');
        expect(tooltip.textContent).toContain('Different values');
        expect(tooltip.textContent).toContain('Sample A: 3');
        expect(tooltip.textContent).toContain('Sample B: -2');

        await fireEvent.input(view.getByRole('spinbutton', { name: 'Coarse tune' }), {
            target: { value: '-2' },
        });
        expect(label.hasAttribute('data-different')).toBe(false);
        expect(label.querySelector('[aria-label="Different values"]')).toBeNull();
        await fireEvent.input(view.getByRole('spinbutton', { name: 'Coarse tune' }), {
            target: { value: '12' },
        });
        expect(label.getAttribute('data-different')).toBe('true');

        await view.rerender({ sample: 'B' });
        await view.findByRole('group', { name: /^Sample: Sample B(?: \(unsaved changes\))?$/ });
        expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('-2');
        expect((view.getByRole('button', { name: 'Undo Sample edit' }) as HTMLButtonElement).disabled).toBe(true);
        expect(view.getByText('Comparing 2 Samples').getAttribute('title')).toBe('Editing Sample B only');
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
    });
});
