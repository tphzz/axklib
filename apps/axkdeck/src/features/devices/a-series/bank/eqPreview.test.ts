import { fireEvent, render } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
import { eqResponse } from '../sample/eqModel';
import SampleEqGraph from '../sample/SampleEqGraph.svelte';
import { BankDraft } from './draft.svelte';

vi.mock('../sample/eqModel', async (original) => {
    const actual = await original<typeof import('../sample/eqModel')>();
    return { ...actual, eqResponse: vi.fn(actual.eqResponse) };
});
beforeEach(() => {
    vi.mocked(eqResponse).mockClear();
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
});

describe('Bank EQ preview source', () => {
    it.each([true, false])(
        'uses the member vector while inherited, not bank coefficients (native bank: %s)',
        async (native) => {
            const values = {
                sample_eq_type: 0,
                sample_eq_frequency: 30,
                sample_eq_gain_db: 3,
                sample_eq_width_tenths: 20,
            };
            const draft = new BankDraft(values, [
                {
                    id: 49,
                    selectors: native ? [49, 50, 51] : [49, 50, 51, 85],
                    activeSelectors: [],
                    keys: Object.keys(values).filter((key) => !native || key !== 'sample_eq_type'),
                },
            ]);
            draft.member = { ...values, sample_eq_type: native ? 2 : 0 };
            const bankVector = [-15904, 7738, 8192, 15904, -7738];
            const memberVector = [-15904, 7738, 8193, 15904, -7738];
            const document = {
                draft,
                detail: {
                    editing: {
                        ...sampleFormatFixture(native ? 'A3000_188' : 'A4000_A5000_224'),
                        blockedParameters: [],
                        eqCoefficients: bankVector,
                    },
                },
                previewDetail: {
                    editing: {
                        ...sampleFormatFixture(native ? 'A4000_A5000_224' : 'A3000_188'),
                        eqCoefficients: memberVector,
                    },
                },
            } as unknown as ObjectEditorDocument;
            const view = render(SampleEqGraph, { document, disabled: false });
            const handle = view.getByRole('button', { name: /EQ frequency \/ gain/ });
            expect(handle.getAttribute('aria-label')).toContain('Preview sample values');
            await fireEvent.click(view.getByRole('button', { name: 'Show coefficient response' }));
            expect(eqResponse).toHaveBeenCalled();
            expect(vi.mocked(eqResponse).mock.calls.every(([vector]) => vector === memberVector)).toBe(true);
            // A parameter change must use newly derived bank coefficients, even if
            // the inherited member had the same named values and a different vector.
            vi.mocked(eqResponse).mockClear();
            flushSync(() => draft.set('sample_eq_gain_db', 4));
            expect(eqResponse).toHaveBeenCalled();
            expect(
                vi.mocked(eqResponse).mock.calls.every(([vector]) => vector !== memberVector && vector !== bankVector),
            ).toBe(true);
            expect(draft.values.sample_eq_type).toBe(0);
            vi.mocked(eqResponse).mockClear();
            flushSync(() => draft.clearOverride('sample_eq_gain_db'));
            expect(vi.mocked(eqResponse).mock.calls.every(([vector]) => vector === memberVector)).toBe(true);
        },
    );
});
