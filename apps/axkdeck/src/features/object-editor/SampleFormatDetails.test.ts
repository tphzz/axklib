import { fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import type { SampleStructureItem } from '../../lib/types';
import { sampleFormatFixture } from '../../test/sampleFormatFixture';
import SampleFormatDetails from './SampleFormatDetails.svelte';

describe('bank stored format', () => {
    it('starts collapsed and separates the bank badge from unique member formats and unresolved references', async () => {
        const native = sampleFormatFixture('A3000_188').sampleFormat;
        const later = sampleFormatFixture().sampleFormat;
        const members = [native, later, later, undefined].map((format, index) => ({
            objectId: ['native', 'later', 'later', 'unknown'][index],
            object: { sampleFormat: format },
        })) as SampleStructureItem[];
        const view = render(SampleFormatDetails, { format: native, bank: true, members, unresolved: 2 });
        const heading = view.getByRole('button', { name: /Stored format/ });
        expect(heading.getAttribute('aria-expanded')).toBe('false');
        expect(view.getByText('a3k')).toBeTruthy();
        await fireEvent.click(heading);
        const region = view.getByRole('region', { name: 'Sample Bank storage format' });
        expect(region.textContent).toContain('1 a3k, 1 a4k/a5k');
        expect(region.textContent).toContain('Unknown member formats');
        expect(region.textContent).toContain('Unresolved member references');
        expect(region.textContent).toContain('Member Samples retain their own formats');
    });
});
