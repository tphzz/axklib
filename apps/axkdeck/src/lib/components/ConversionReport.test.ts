import { fireEvent, render, within } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import type { ConversionReport as Report } from '../conversions';
import { fitsCapacity } from '../../test/samplerCapacityFixture';
import ConversionReport from './ConversionReport.svelte';

function report(): Report {
    return {
        profileId: 'yamaha.tx16w-to-a-series',
        fidelity: 'BEST',
        executable: true,
        destination: { imageId: 'image', documentId: 'document', expectedRevision: 1 },
        capacity: fitsCapacity,
        offset: 0,
        actionCount: 201,
        issueCount: 101,
        actions: Array.from({ length: 201 }, (_, index) => ({
            name: `Wave ${index}`,
            kind: 'wave-data',
            action: 'create',
            sourceObject: `wave-${index}`,
            details: [
                { label: 'Sample rate', value: '32000' },
                { label: 'Loop start', value: '24474' },
            ],
        })),
        issues: Array.from({ length: 101 }, (_, index) => ({
            id: `loss-${index}`,
            code: 'loss',
            impact: 'LOSS',
            message: `Mapping loss ${index}`,
            sourceObject: `Voice ${index}`,
            sourceParameter: 'timbre_slot',
            targetObject: '',
            targetParameter: '',
        })),
    };
}

describe('Conversion report panes', () => {
    it('pages objects and mapping details independently and retains per-issue consent', async () => {
        const view = render(ConversionReport, { report: report() });
        const contents = within(view.getByRole('region', { name: 'Contents' }));
        const review = within(view.getByRole('region', { name: 'Review' }));
        const firstConsent = review.getByRole('checkbox', { name: /Mapping loss 0\b/ }) as HTMLInputElement;
        await fireEvent.click(firstConsent);
        await fireEvent.click(contents.getByRole('button', { name: 'Next objects page' }));
        expect(contents.queryByText('Wave 0')).toBeNull();
        expect(contents.getByText('Wave 100')).toBeTruthy();
        expect(firstConsent.checked).toBe(true);
        await fireEvent.click(review.getByRole('button', { name: 'Next mapping details page' }));
        expect(review.getByRole('checkbox', { name: /Mapping loss 100\b/ })).toBeTruthy();
        expect(contents.getByText('Wave 100')).toBeTruthy();
        expect(review.getByRole('button', { name: 'Next mapping details page' }).hasAttribute('disabled')).toBe(true);
        await fireEvent.click(review.getByRole('button', { name: 'Previous mapping details page' }));
        expect((review.getByRole('checkbox', { name: /Mapping loss 0\b/ }) as HTMLInputElement).checked).toBe(true);
    });

    it('prioritizes blockers and never allows Strict loss consent to override fidelity', () => {
        const data = report();
        data.fidelity = 'STRICT';
        data.executable = false;
        data.issues.push({ ...data.issues[0], id: 'blocker', impact: 'BLOCKER', message: 'Missing Wave PIANO' });
        data.issueCount++;
        const view = render(ConversionReport, { report: data });
        expect(view.getByText(/Missing Wave PIANO/)).toBeTruthy();
        const review = view.getByRole('region', { name: 'Review' });
        expect(review.textContent!.indexOf('Missing Wave PIANO')).toBeLessThan(
            review.textContent!.indexOf('Mapping loss 0'),
        );
        expect(view.getAllByRole('checkbox').every((checkbox) => checkbox.hasAttribute('disabled'))).toBe(true);
        expect(view.getByText('Strict fidelity does not allow loss or uncertainty.')).toBeTruthy();
    });

    it('keeps report navigation available but locks every consent while submitting', () => {
        const view = render(ConversionReport, { report: report(), disabled: true });
        expect(view.getAllByRole('checkbox').every((checkbox) => checkbox.hasAttribute('disabled'))).toBe(true);
        expect(view.getByRole('button', { name: 'Next objects page' }).hasAttribute('disabled')).toBe(false);
    });
});
