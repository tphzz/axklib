import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { AuditionState } from '../../../lib/audio/auditionController';
import { nativeDocumentFixture, tx16wDocument } from '../../../test/nativeDocumentFixture';
import { fixtureFrames } from '../../../test/sampleEditorAudio';
import PresentationAudioHarness from './Presentation.audio.testHarness.svelte';
import { ViewMemory } from '../../workspace/viewMemory';

const audio = vi.hoisted(() => ({
    play: vi.fn(),
    seek: vi.fn(),
    stop: vi.fn(),
    dispose: vi.fn(),
    invalidate: vi.fn(),
    prefetch: vi.fn(),
}));
vi.mock('../../../lib/audio/auditionController', () => ({
    AuditionController: class {
        constructor(
            _transport: unknown,
            private publishState: (state: AuditionState) => void,
        ) {}
        async play(session: number, objectId: string) {
            audio.play(session, objectId);
            this.publishState({ objectId, status: 'playing', playheadFrame: 0 });
        }
        async stop() {
            audio.stop();
            this.publishState({ objectId: null, status: 'idle', playheadFrame: 0 });
        }
        seek = audio.seek;
        dispose = audio.dispose;
        invalidateSession = audio.invalidate;
        prefetch = audio.prefetch;
    },
}));

beforeEach(() => vi.clearAllMocks());
async function setup() {
    const transport = nativeDocumentFixture();
    const preview = vi.spyOn(transport, 'preview');
    const nativeObjects = vi.spyOn(transport, 'nativeObjects');
    const details = vi.spyOn(transport, 'nativeObjectDetail');
    const props = {
        transport,
        sessionId: 1,
        revision: 1,
        document: tx16wDocument,
        sources: [],
        remember: new ViewMemory().forDocument(1, tx16wDocument.id),
        onrevealFiles: vi.fn(),
    };
    const view = render(PresentationAudioHarness, props);
    await fireEvent.click(screen.getByRole('button', { name: 'Waves' }));
    const row = await screen.findByRole('group', { name: 'Wave 20 Wave Data' });
    await waitFor(() => expect(row.textContent).toContain('33,333 Hz'));
    return { ...view, row, preview, nativeObjects, details };
}

describe('TX16W shared wave controls', () => {
    it('loads neutral waveform previews and retains native precision and approximate-rate metadata', async () => {
        const { row, preview } = await setup();
        expect(preview).toHaveBeenCalledWith(1, 'wave-20', expect.any(Number), {
            signal: expect.any(AbortSignal),
            readIntent: 'SELECTED',
        });
        expect(row.querySelector('canvas')).not.toBeNull();
        expect(row.textContent).toContain('12-bit');
        expect(row.textContent).toContain('Approx.');
        await fireEvent.click(within(row).getByRole('button', { name: 'Inspect Wave 20' }));
        await screen.findByRole('heading', { name: 'Wave 20' });
        expect(await screen.findByText(/Sample rate is uncertain; audition uses 33,333 Hz/)).toBeTruthy();
        expect(audio.play).not.toHaveBeenCalled();
    });

    it('selects through the blank-area overlay without playing and keeps Play and Seek independent', async () => {
        const { row, details } = await setup();
        const select = within(row).getByRole('button', { name: 'Inspect Wave 20' });
        expect(select.textContent).toBe('');
        await fireEvent.click(select);
        expect(select.getAttribute('aria-pressed')).toBe('true');
        expect(audio.play).not.toHaveBeenCalled();
        await fireEvent.click(within(row).getByRole('button', { name: 'Play Wave 20' }));
        expect(audio.play).toHaveBeenCalledExactlyOnceWith(1, 'wave-20');
        const seek = within(row).getByRole('slider', { name: 'Seek Wave 20' });
        const detailCount = details.mock.calls.length;
        await fireEvent.input(seek, { target: { value: '0.5' } });
        expect(audio.seek).toHaveBeenCalledExactlyOnceWith(Math.floor(fixtureFrames / 2));
        expect(audio.play).toHaveBeenCalledTimes(1);
        expect(details).toHaveBeenCalledTimes(detailCount);
        await fireEvent.input(seek, { target: { value: '0.6' } });
        expect(details).toHaveBeenCalledTimes(detailCount);
        expect((seek as HTMLInputElement).min).toBe('0');
        expect((seek as HTMLInputElement).max).toBe('1');
        expect(seek.getAttribute('aria-valuetext')).toContain('s');
        await fireEvent.click(within(row).getByRole('button', { name: 'Stop Wave 20' }));
        expect(within(row).getByRole('button', { name: 'Play Wave 20' })).toBeTruthy();
    });

    it('stops audition when the Device presentation is hidden and does not restart on return', async () => {
        const view = await setup();
        await fireEvent.click(within(view.row).getByRole('button', { name: 'Play Wave 20' }));
        const stopCount = audio.stop.mock.calls.length;
        await fireEvent.click(screen.getByRole('button', { name: 'Switch presentation visibility' }));
        expect(audio.stop.mock.calls.length).toBeGreaterThan(stopCount);
        await fireEvent.click(screen.getByRole('button', { name: 'Switch presentation visibility' }));
        expect(await screen.findByRole('button', { name: 'Play Wave 20' })).toBeTruthy();
        expect(audio.play).toHaveBeenCalledTimes(1);
        expect(view.nativeObjects).toHaveBeenCalledOnce();
        view.unmount();
        expect(audio.dispose).toHaveBeenCalledOnce();
    });
});
