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

const decision = 'Create an audition Program from Piano Voice';

function reportWithDecision(): Report {
    const data = report();
    data.issues.push({ ...data.issues[0], impact: 'UNCERTAINTY', id: 'voice', decision });
    data.issueCount++;
    return data;
}

describe('Conversion report panes', () => {
    it('reports losses and ordinary defaults without consent on any page', async () => {
        const data = report();
        data.issues.push({ ...data.issues[0], id: 'default', impact: 'UNCERTAINTY', message: 'Writer default' });
        data.issueCount++;
        const view = render(ConversionReport, { report: data });
        const region = view.getByRole('region', { name: 'Review' });
        expect(region.textContent).toContain('Conversion notes');
        expect(within(region).queryByRole('checkbox')).toBeNull();
        await fireEvent.click(view.getByRole('button', { name: 'Next mapping details page' }));
        expect(region.textContent).toContain('Mapping loss 100');
        expect(region.textContent).toContain('Writer default');
        expect(within(region).queryByRole('checkbox')).toBeNull();
    });

    it('pages output and notes independently while retaining the explicit mapping decision', async () => {
        const view = render(ConversionReport, { report: reportWithDecision() });
        const contents = within(view.getByRole('region', { name: 'Contents' }));
        const region = view.getByRole('region', { name: 'Review' });
        const review = within(region);
        const control = review.getByRole('checkbox', { name: decision }) as HTMLInputElement;
        expect(control.checked).toBe(false);
        await fireEvent.click(control);
        await fireEvent.click(contents.getByRole('button', { name: 'Next objects page' }));
        expect(contents.queryByText('Wave 0')).toBeNull();
        expect(contents.getByText('Wave 100')).toBeTruthy();
        expect(control.checked).toBe(true);
        await fireEvent.click(review.getByRole('button', { name: 'Next mapping details page' }));
        expect(region.textContent).toContain('Mapping loss 100');
        expect(contents.getByText('Wave 100')).toBeTruthy();
        expect(review.getByRole('button', { name: 'Next mapping details page' }).hasAttribute('disabled')).toBe(true);
        await fireEvent.click(review.getByRole('button', { name: 'Previous mapping details page' }));
        expect((review.getByRole('checkbox', { name: decision }) as HTMLInputElement).checked).toBe(true);
    });

    it('prioritizes blockers and offers no acknowledgement to override Strict fidelity', () => {
        const data = report();
        data.fidelity = 'STRICT';
        data.executable = false;
        data.issues.push({ ...data.issues[0], id: 'blocker', impact: 'BLOCKER', message: 'Missing Wave PIANO' });
        data.issueCount++;
        const view = render(ConversionReport, { report: data });
        const review = view.getByRole('region', { name: 'Review' });
        expect(review.textContent).toContain('Missing Wave PIANO');
        expect(review.textContent).toMatch(/Strict/);
        expect(within(review).queryByRole('checkbox')).toBeNull();
    });

    it('keeps navigation available but locks explicit decisions while submitting', () => {
        const view = render(ConversionReport, { report: reportWithDecision(), disabled: true });
        expect(view.getByRole('checkbox', { name: decision }).hasAttribute('disabled')).toBe(true);
        expect(view.getByRole('button', { name: 'Next objects page' }).hasAttribute('disabled')).toBe(false);
    });
});
