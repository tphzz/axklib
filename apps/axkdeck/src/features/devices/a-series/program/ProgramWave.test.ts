import { act, fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import { programEditorFixture } from '../../../../test/programEditorFixture';
import ProgramWave from './ProgramWave.svelte';

describe('Program StepWave graph', () => {
    it('edits a step by keyboard as one undo gesture without changing inactive steps', async () => {
        const { document } = programEditorFixture();
        const view = render(ProgramWave, { document, step: true });
        const handle = view.getByRole('button', { name: 'Step 1: 64' });
        await fireEvent.keyDown(handle, { key: 'ArrowUp' });
        await fireEvent.keyDown(handle, { key: 'ArrowUp' });
        await fireEvent.keyUp(handle, { key: 'ArrowUp' });
        expect(document.draft.values['step_wave.values.1']).toBe(66);
        expect(document.draft.values['step_wave.values.9']).toBe(64);
        expect(view.queryByRole('button', { name: 'Step 9: 64' })).toBeNull();
        await act(() => document.draft.undo());
        expect(document.draft.values['step_wave.values.1']).toBe(64);
    });

    it('disables every graph handle while the document is inactive', () => {
        const { document } = programEditorFixture();
        const view = render(ProgramWave, { document, step: true, disabled: true });
        expect(view.getByRole('button', { name: 'Step 1: 64' })).toHaveProperty('disabled', true);
    });
});
