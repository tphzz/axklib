import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import CapacityRefreshHarness from '../../../test/CapacityRefreshHarness.svelte';
import { capacityRefreshFixture } from '../../../test/capacityRefreshFixture.svelte';

const panel = () => screen.getByRole('button', { name: 'Sampler Capacity' });
const profiles = () => document.querySelectorAll('.capacity-profile');
async function setup() {
    const fixture = capacityRefreshFixture();
    render(CapacityRefreshHarness, { fixture });
    await waitFor(() => expect(document.querySelector('[data-tree-id="volume-1"]')).toBeTruthy());
    await fireEvent.click(document.querySelector('[data-tree-id="volume-1"]')!);
    await waitFor(() => expect(profiles()).toHaveLength(2));
    return fixture;
}

describe('workspace capacity after import refresh', () => {
    it.each(['floppy', 'package', 'audio'])(
        'refreshes a collapsed table after %s import without another tree click',
        async (type) => {
            const fixture = await setup();
            expect(panel().getAttribute('aria-expanded')).toBe('false');
            await fireEvent.click(screen.getByRole('button', { name: `Complete ${type} import` }));
            const scope = type === 'audio' ? 'volume-1' : 'volume-2';
            await waitFor(() => expect(fixture.calls).toContain(`2:${scope}`));
            await waitFor(() => expect(profiles()).toHaveLength(2));
            if (type !== 'audio') expect(fixture.calls).not.toContain('2:volume-1');
            await fireEvent.click(panel());
            const later = screen.getByRole('group', { name: 'A4000/A5000 capacity' });
            expect(within(later).getByText('123.1 / 768 KiB')).toBeTruthy();
            expect(screen.queryByRole('button', { name: 'Retry capacity inspection' })).toBeNull();
            if (type === 'package') expect(screen.getByRole('heading', { name: 'New import' })).toBeTruthy();
        },
    );

    it('loads while expanded, honours a pending manual choice and ignores the earlier report', async () => {
        const fixture = await setup();
        fixture.hold = true;
        await fixture.refresh();
        await waitFor(() => expect(fixture.pendingCount).toBe(1));
        await fireEvent.click(panel());
        expect(screen.getByText('Inspecting capacity...')).toBeTruthy();
        await fixture.refresh('DOES_NOT_FIT');
        await waitFor(() => expect(fixture.pendingCount).toBe(2));
        await fireEvent.click(panel());
        await fireEvent.click(panel());
        fixture.release();
        await waitFor(() => expect(profiles()).toHaveLength(2));
        expect(panel().getAttribute('aria-expanded')).toBe('false');
        await fireEvent.click(panel());
        expect(screen.getAllByText('Does not fit')).toHaveLength(2);
    });

    it('expands for a new violation and retains genuine retry', async () => {
        await setup();
        await fireEvent.click(screen.getByRole('button', { name: 'Exceed capacity' }));
        await waitFor(() => expect(panel().getAttribute('aria-expanded')).toBe('true'));
        await fireEvent.click(screen.getByRole('button', { name: 'Fail inspection' }));
        await waitFor(() =>
            expect(document.querySelector('[role="alert"]')?.textContent).toBe('Capacity service disconnected'),
        );
        expect(panel().getAttribute('aria-expanded')).toBe('false');
        await fireEvent.click(panel());
        await fireEvent.click(screen.getByRole('button', { name: 'Restore inspection' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Retry capacity inspection' }));
        await waitFor(() => expect(profiles()).toHaveLength(2));
        expect(panel().getAttribute('aria-expanded')).toBe('true');
    });

    it('keeps explicit Program inspection across a refresh', async () => {
        await setup();
        await fireEvent.click(document.querySelector('.program-row')!);
        expect(screen.queryByRole('complementary', { name: 'Volume inspector' })).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Complete floppy import' }));
        await waitFor(() => expect(document.querySelector('[data-tree-id="volume-2"]')).toBeTruthy());
        expect(screen.queryByRole('complementary', { name: 'Volume inspector' })).toBeNull();
    });
});
