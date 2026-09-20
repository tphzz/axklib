import { sampleFormatFixture } from '../../../test/sampleFormatFixture';
import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import type { ObjectDetail, SamplerObject } from '../../../lib/transport';
import type { SampleStorageFormat } from '../../../lib/objectEditing';
import type { SampleStructureItem } from '../../../lib/types';
import SampleCollectionHarness from '../../../test/SampleCollectionHarness.svelte';
import { ObjectEditorWorkflow } from '../../object-editor/workflow.svelte';

function sample(id: string, format: SampleStorageFormat): SampleStructureItem {
    const name = `Sample ${id}`;
    const object: SamplerObject = {
        key: id,
        objectType: 'SBNK',
        name,
        partitionIndex: 0,
        partitionName: 'Partition 0',
        volumeName: 'Volume',
        categoryName: 'Samples',
        objectEncoding: 'current',
        directoryEntryName: name,
        sfsId: 0,
        storedSizeBytes: 356,
        sizeWithDependenciesBytes: 610304,
        sampleRate: 44100,
        rootKey: 60,
        storedFrameCount: 84000,
        waveStartFrame: 0,
        waveLengthFrames: 84000,
        storageState: 'COMPLETE',
        sampleWidthBytes: 2,
        sampleFormat: sampleFormatFixture(format).sampleFormat,
    };
    return { id, objectId: id, name, objectType: 'SBNK', object, membershipLabel: 'Standalone' };
}

function setup(initiallySelected = ['A', 'B'], format: SampleStorageFormat = 'A3000_188') {
    const transport = {
        objectDetail: vi.fn(
            async (_: number, id: string) =>
                ({
                    image: { revision: 1 },
                    object: { id, key: id, name: `Sample ${id}` },
                    editing: {
                        profile: 'a-series/sample',
                        editable: true,
                        reason: '',
                        payloadSha256: 'a'.repeat(64),
                        parameters: { level: 100, pan: 0, loop_mode: 4, loop_start_frame: 0, loop_length_frames: 100 },
                        playbackWindow: { start_frame: 0, length_frames: 100 },
                        maximumFrames: 100,
                        canEditPlayback: true,
                        eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
                        blockedParameters: [],
                        blockedParameterReasons: {},
                        ...sampleFormatFixture(format),
                        unavailableParameters: {},
                        partitionIndex: 0,
                        volumeName: 'Volume',
                        sources: [],
                    },
                }) as unknown as ObjectDetail,
        ),
        startObjectParameterEdit: vi.fn().mockResolvedValue({ jobId: 7, status: 'queued' }),
        startSampleDuplication: vi.fn().mockResolvedValue({ jobId: 8, status: 'queued' }),
        startSampleFormatConversion: vi.fn(),
        waitForJob: vi.fn().mockResolvedValue({ jobId: 8, status: 'completed' }),
    };
    const workflow = new ObjectEditorWorkflow({
        transport,
        refresh: vi.fn().mockResolvedValue(undefined),
        stopPlayback: vi.fn(),
        status: vi.fn(),
    });
    const view = render(SampleCollectionHarness, {
        workflow,
        samples: ['A', 'B', 'C'].map((id) => sample(id, format)),
        initiallySelected,
    });
    return { view, workflow, transport };
}

describe('Sample collection editor selection', () => {
    it('activates the remaining Sample when Ctrl-toggle removes the active Sample from a two-Sample selection', async () => {
        const { view, workflow } = setup();
        await waitFor(() => expect(workflow.comparison.count).toBe(2));

        await fireEvent.click(view.getByRole('button', { name: 'Inspect Sample A' }), { ctrlKey: true });

        expect(view.getByLabelText('Selected Samples').textContent).toBe('B');
        expect(view.getByLabelText('Active Sample').textContent).toBe('B');
        expect(view.getByLabelText('Lower zone open').textContent).toBe('true');
        expect(view.getByRole('button', { name: 'Inspect Sample A' }).getAttribute('aria-pressed')).toBe('false');
        expect(view.getByRole('button', { name: 'Inspect Sample B' }).getAttribute('aria-pressed')).toBe('true');
        expect(workflow.comparison.count).toBe(0);
    });

    it('keeps the current Sample active and the lower zone closed when removing only the other selection', async () => {
        const { view } = setup();

        await fireEvent.click(view.getByRole('button', { name: 'Inspect Sample B' }), { ctrlKey: true });

        expect(view.getByLabelText('Selected Samples').textContent).toBe('A');
        expect(view.getByLabelText('Active Sample').textContent).toBe('A');
        expect(view.getByLabelText('Lower zone open').textContent).toBe('false');
    });
});

describe('Sample collection Duplicate context action', () => {
    it.each([
        ['A3000_188', 'a4k/a5k'],
        ['A4000_A5000_224', 'a3k'],
    ] as const)('names the conversion target for %s without submitting a write', async (format, target) => {
        const { view, workflow, transport } = setup(['A'], format);
        const row = view.getByRole('button', { name: 'Inspect Sample A' });
        await fireEvent.keyDown(row, { key: 'F10', shiftKey: true });
        const action = view.getByRole('menuitem', { name: `Convert to ${target} sample format...` });
        await fireEvent.click(action);
        await waitFor(() => expect(workflow.conversionDocument?.detail?.object.id).toBe('A'));
        expect(view.queryByRole('menu')).toBeNull();
        expect(transport.startSampleFormatConversion).not.toHaveBeenCalled();
        expect(view.getByLabelText('Selected Samples').textContent).toBe('A');
    });

    it('opens duplication for a single Sample through mouse and keyboard context menus without writing', async () => {
        const { view, workflow, transport } = setup(['A']);
        const row = view.getByRole('button', { name: 'Inspect Sample A' });
        await fireEvent.contextMenu(row, { clientX: 100, clientY: 100 });
        await fireEvent.click(view.getByRole('menuitem', { name: 'Duplicate...' }));

        await waitFor(() => expect(workflow.duplication.source?.detail?.object.id).toBe('A'));
        expect(workflow.duplication.visible).toBe(true);
        expect(workflow.duplication.name).toBe('Sample A Copy');
        expect(transport.startSampleDuplication).not.toHaveBeenCalled();
        workflow.duplication.close();

        await fireEvent.keyDown(row, { key: 'F10', shiftKey: true });
        expect(view.getByRole('menuitem', { name: 'Duplicate...' })).toBeTruthy();
    });

    it('omits Duplicate for multiple selected Samples and retains their selection', async () => {
        const { view, workflow } = setup();
        await fireEvent.contextMenu(view.getByRole('button', { name: 'Inspect Sample A' }));

        expect(view.getByRole('menu')).toBeTruthy();
        expect(view.queryByRole('menuitem', { name: 'Duplicate...' })).toBeNull();
        expect(view.getByLabelText('Selected Samples').textContent).toBe('A,B');
        expect(workflow.duplication.visible).toBe(false);
    });

    it('duplicates only a newly right-clicked Sample after replacing a different multi-selection', async () => {
        const { view, workflow } = setup();
        await fireEvent.contextMenu(view.getByRole('button', { name: 'Inspect Sample C' }));
        expect(view.getByLabelText('Selected Samples').textContent).toBe('C');
        await fireEvent.click(view.getByRole('menuitem', { name: 'Duplicate...' }));

        await waitFor(() => expect(workflow.duplication.source?.detail?.object.id).toBe('C'));
        expect(workflow.duplication.name).toBe('Sample C Copy');
    });

    it('omits Duplicate when mutations are unavailable or the view is Sample Bank members', async () => {
        const { view } = setup(['A']);
        await view.rerender({ mutationsAvailable: false });
        await fireEvent.contextMenu(view.getByRole('button', { name: 'Inspect Sample A' }));
        expect(view.getByRole('menu')).toBeTruthy();
        expect(view.queryByRole('menuitem', { name: 'Duplicate...' })).toBeNull();

        await fireEvent.keyDown(window, { key: 'Escape' });
        await view.rerender({ mutationsAvailable: true, view: 'sample-banks' });
        await fireEvent.contextMenu(view.getByRole('button', { name: 'Inspect Sample A' }));
        expect(view.queryByRole('menuitem', { name: 'Duplicate...' })).toBeNull();
    });
});
