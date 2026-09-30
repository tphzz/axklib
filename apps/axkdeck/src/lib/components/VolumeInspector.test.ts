import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import VolumeInspectorHarness from '../../test/VolumeInspectorHarness.svelte';
import { volumeCapacityFixture } from '../../test/volumeCapacityFixture';
import type { VolumeCapacityInspection } from '../volumeInspections';

const panel = () => screen.getByRole('button', { name: 'Sampler Capacity' });

describe('Volume capacity inspector', () => {
    it.each(['FITS'] as const)('starts collapsed for %s', async (status) => {
        render(VolumeInspectorHarness, { report: volumeCapacityFixture(status) });
        await waitFor(() => expect(document.querySelector('.capacity-profile')).toBeTruthy());
        expect(panel().getAttribute('aria-expanded')).toBe('false');
    });
    it.each([
        ['DOES_NOT_FIT', 'FITS'],
        ['FITS', 'DOES_NOT_FIT'],
    ] as const)('expands for a violation in either generation (%s / %s)', async (first, later) => {
        render(VolumeInspectorHarness, { report: volumeCapacityFixture(first, later) });
        await waitFor(() => expect(panel().getAttribute('aria-expanded')).toBe('true'));
    });
    it('shows both generations without controls or duplicate failure prose', async () => {
        render(VolumeInspectorHarness, { report: volumeCapacityFixture('DOES_NOT_FIT') });
        await waitFor(() => expect(panel().getAttribute('aria-expanded')).toBe('true'));
        expect(screen.getByRole('heading', { name: 'A3000' })).toBeTruthy();
        const later = screen.getByRole('group', { name: 'A4000/A5000 capacity' });
        expect(within(later).getByText('\u2265 840.5 / 768 KiB')).toBeTruthy();
        expect(screen.getAllByText('Does not fit')).toHaveLength(2);
        expect(screen.queryByRole('button', { name: 'a3k' })).toBeNull();
        expect(screen.queryByRole('button', { name: 'a4k/a5k' })).toBeNull();
        expect(screen.queryByText('Load peak')).toBeNull();
        expect(screen.queryByText('Load baseline')).toBeNull();
        expect(screen.queryByText(/proven resident minimum exceeds/)).toBeNull();
    });
    it('uses loading demand rather than the smaller final allocation and retains exact bytes in help', async () => {
        render(VolumeInspectorHarness);
        await waitFor(() => expect(document.querySelector('.capacity-profile')).toBeTruthy());
        await fireEvent.click(panel());
        const later = screen.getByRole('group', { name: 'A4000/A5000 capacity' });
        expect(within(later).getByText('121.1 / 768 KiB')).toBeTruthy();
        await fireEvent.focus(within(later).getByRole('button', { name: 'Parameter memory' }));
        expect(screen.getByRole('tooltip').textContent).toContain('124,000');
        expect(screen.getByRole('tooltip').textContent).toContain('786,432');
    });
    it('honours manual collapse but resets the default for changed volume or revision', async () => {
        const report = volumeCapacityFixture('DOES_NOT_FIT');
        const view = render(VolumeInspectorHarness, { report });
        await waitFor(() => expect(panel().getAttribute('aria-expanded')).toBe('true'));
        await fireEvent.click(panel());
        await view.rerender({ report });
        expect(panel().getAttribute('aria-expanded')).toBe('false');
        await view.rerender({ volumeId: 'another' });
        await waitFor(() => expect(panel().getAttribute('aria-expanded')).toBe('true'));
        await fireEvent.click(panel());
        await view.rerender({ revision: 2 });
        await waitFor(() => expect(panel().getAttribute('aria-expanded')).toBe('true'));
    });
    it('does not overwrite an explicit choice made before asynchronous inspection finishes', async () => {
        let resolve!: (value: VolumeCapacityInspection) => void;
        const pending = new Promise<VolumeCapacityInspection>((done) => {
            resolve = done;
        });
        render(VolumeInspectorHarness, { pending });
        expect(panel().getAttribute('aria-expanded')).toBe('false');
        await fireEvent.click(panel());
        resolve({ imageId: 'image', revision: 1, contentScopeId: 'volume', report: volumeCapacityFixture() });
        await waitFor(() => expect(document.querySelector('.capacity-profile')).toBeTruthy());
        expect(panel().getAttribute('aria-expanded')).toBe('true');
    });
    it('leaves inspection failures collapsed and exposes retry when manually opened', async () => {
        render(VolumeInspectorHarness, { error: 'Capacity unavailable' });
        await waitFor(() => expect(document.querySelector('[role="alert"]')).toBeTruthy());
        expect(panel().getAttribute('aria-expanded')).toBe('false');
        await fireEvent.click(panel());
        expect(screen.getByRole('button', { name: 'Retry capacity inspection' })).toBeTruthy();
    });
    it('honours manual collapse before an asynchronous violation is reported', async () => {
        let resolve!: (value: VolumeCapacityInspection) => void;
        const pending = new Promise<VolumeCapacityInspection>((done) => {
            resolve = done;
        });
        render(VolumeInspectorHarness, { pending });
        await fireEvent.click(panel());
        await fireEvent.click(panel());
        resolve({
            imageId: 'image',
            revision: 1,
            contentScopeId: 'volume',
            report: volumeCapacityFixture('DOES_NOT_FIT'),
        });
        await waitFor(() => expect(document.querySelector('.capacity-profile')).toBeTruthy());
        expect(panel().getAttribute('aria-expanded')).toBe('false');
    });
    it.each(['LOAD_ALLOCATION_FAILED', 'PARAMETER_POOL_EXHAUSTED'])(
        'retains and deduplicates the distinct %s explanation',
        async (code) => {
            const report = volumeCapacityFixture('FITS', 'DOES_NOT_FIT');
            report.profiles[1].reasons = [
                { code, message: 'Distinct reason.' },
                { code, message: 'Distinct reason.' },
            ];
            render(VolumeInspectorHarness, { report });
            await waitFor(() => expect(document.querySelector('.capacity-profile')).toBeTruthy());
            if (panel().getAttribute('aria-expanded') === 'false') await fireEvent.click(panel());
            const later = screen.getByRole('group', { name: 'A4000/A5000 capacity' });
            expect(within(later).getAllByText('Distinct reason.')).toHaveLength(1);
        },
    );
});
