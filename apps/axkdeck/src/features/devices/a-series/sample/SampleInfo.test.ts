import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import { fireEvent, render } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import SampleInfo from './SampleInfo.svelte';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';

beforeEach(() =>
    vi.stubGlobal(
        'ResizeObserver',
        class {
            observe() {}
            disconnect() {}
        },
    ),
);

describe('Sample source metadata', () => {
    it('keeps source duration beside the rate instead of reserving a footer row', async () => {
        const draft = new EditorDraft({
            loop_tempo_hundredths: 12000,
            wave_start_velocity_sensitivity: 0,
            loop_length_frames: 44100,
        });
        const document = {
            preferencesScope: {},
            draft,
            detail: {
                editing: {
                    maximumFrames: 152076,
                    blockedParameters: [],
                    blockedParameterReasons: {},
                    ...sampleFormatFixture(),
                    unavailableParameters: {},
                },
            },
            inputErrors: {},
        } as unknown as ObjectEditorDocument;
        const view = render(SampleInfo, { document, rate: 44100, disabled: false, onmonitor: vi.fn() });
        const duration = view.getByRole('button', { name: 'Source: 3.448 s' });
        expect(duration.closest('.editor-toolbar')?.textContent).toContain('44,100 Hz');
        expect(view.container.querySelector('.settings-summary')).toBeNull();
        await fireEvent.focus(duration);
        expect(view.getByRole('tooltip').textContent).toContain('independent of playback trimming and loop boundaries');
        flushSync(() => draft.patch({ wave_length_frames: 1000, loop_length_frames: 500 }));
        expect(duration.textContent).toBe('Source: 3.448 s');
    });
});
