import { fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
import VelocityRange from './VelocityRange.svelte';

describe('Sample velocity graph format capabilities', () => {
    it.each([true, false])('permits graph editing only for valid stored values: %s', async (valid) => {
        const format = sampleFormatFixture();
        format.parameterCapabilities.velocity_low!.valid = valid;
        const draft = new EditorDraft({ velocity_low: 30, velocity_high: 127 });
        const document = {
            draft,
            detail: { editing: { ...format, blockedParameters: [] } },
        } as unknown as ObjectEditorDocument;
        const view = render(VelocityRange, { document, fields: [], disabled: false });
        const low = view.getByRole('slider', { name: 'Low velocity boundary' }) as HTMLButtonElement;
        expect(low.disabled).toBe(!valid);
        await fireEvent.keyDown(low, { key: 'ArrowUp' });
        expect(draft.values.velocity_low).toBe(valid ? 31 : 30);
    });
});
