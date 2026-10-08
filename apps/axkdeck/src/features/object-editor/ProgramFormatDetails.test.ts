import { cleanup, fireEvent, render } from '@testing-library/svelte';
import { afterEach, describe, expect, it } from 'vitest';
import { programFormatFixture } from '../../test/programFormatFixture';
import ProgramFormatDetails from './ProgramFormatDetails.svelte';
import ProgramFormatBadge from './ProgramFormatBadge.svelte';

afterEach(cleanup);

describe('Program storage details', () => {
    it('never presents malformed metadata as a normal badge', async () => {
        const view = render(ProgramFormatBadge, { format: { ...programFormatFixture(), structurallyValid: false } });
        expect(view.queryByText('a3k')).toBeNull();
        expect(view.getByText('?')).toBeTruthy();
        await view.rerender({ format: programFormatFixture(true) });
        expect(view.getByText('a4k/a5k')).toBeTruthy();
    });

    it('shows unavailable metrics and explains stored representation through accessible help', async () => {
        const format = {
            ...programFormatFixture(),
            format: 'UNKNOWN' as const,
            structurallyValid: false,
            headerRevision: 3,
            logicalSize: null,
            storedAssignmentCount: null,
            assignmentCapacity: null,
            parameterTailBytes: null,
        };
        const view = render(ProgramFormatDetails, { format });
        const section = view.getByRole('button', { name: /Stored format/ });
        if (section.getAttribute('aria-expanded') !== 'true') await fireEvent.click(section);
        expect(view.getAllByText('Unavailable')).toHaveLength(3);
        expect(view.queryByText('0 bytes')).toBeNull();
        const help = view.getByRole('button', { name: 'Header revision' });
        await fireEvent.focus(help);
        expect(view.getByRole('tooltip').textContent).toContain('stored representation');
        await fireEvent.keyDown(help, { key: 'Escape' });
        expect(view.queryByRole('tooltip')).toBeNull();
        await fireEvent.click(help);
        expect(view.getByRole('tooltip')).toBeTruthy();
        await fireEvent.pointerDown(document.body);
        expect(view.queryByRole('tooltip')).toBeNull();
        await view.rerender({ format: programFormatFixture(true) });
        expect(view.queryByText('Unavailable')).toBeNull();
        expect(view.getByText('176 bytes')).toBeTruthy();
        await view.rerender({ format: { ...programFormatFixture(), headerRevision: 1 } });
        await fireEvent.click(view.getByRole('button', { name: 'Header revision' }));
        expect(view.getByRole('tooltip').textContent).toContain('not original V1 playback');
    });
});
