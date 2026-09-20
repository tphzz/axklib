import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import { fireEvent, render, waitFor, within } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { ObjectDetail } from '../../lib/transport';
import DeviceEditorHostHarness from '../../test/DeviceEditorHostHarness.svelte';
import { ObjectEditorWorkflow } from './workflow.svelte';

beforeEach(() => vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null));

function detail(id: string): ObjectDetail {
    return {
        image: { revision: 1 },
        object: { id, key: id, name: `Sample ${id}` },
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

function setup() {
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
    return { workflow, transport, view: render(DeviceEditorHostHarness, { workflow }) };
}

describe('Sample editor workspace navigation', () => {
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
        await view.findByText('Sample B', { selector: 'strong' });
        expect(view.getByRole('tab', { name: 'Map/Out' }).getAttribute('aria-selected')).toBe('true');
        expect(view.getByRole('button', { name: 'Pitch' }).getAttribute('aria-pressed')).toBe('true');
        const tuneB = view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement;
        expect(tuneB.value).toBe('-2');
        expect((view.getByRole('button', { name: 'Undo Sample edit' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.input(tuneB, { target: { value: '-10' } });

        await view.rerender({ sample: 'A' });
        await view.findByText('Sample A', { selector: 'strong' });
        expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('12');
        await fireEvent.click(view.getByRole('button', { name: 'Undo Sample edit' }));
        expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('3');

        await view.rerender({ sample: 'B' });
        await view.findByText('Sample B', { selector: 'strong' });
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
        await view.findByText('Sample B', { selector: 'strong' });
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
        await view.findByText('Sample B', { selector: 'strong' });
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
        await view.findByText('Sample B', { selector: 'strong' });
        expect((view.getByRole('spinbutton', { name: 'Coarse tune' }) as HTMLInputElement).value).toBe('-2');
        expect((view.getByRole('button', { name: 'Undo Sample edit' }) as HTMLButtonElement).disabled).toBe(true);
        expect(view.getByText('Comparing 2 Samples').getAttribute('title')).toBe('Editing Sample B only');
        expect(transport.startObjectParameterEdit).not.toHaveBeenCalled();
    });
});
