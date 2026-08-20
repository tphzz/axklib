import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import AssignSampleBankDialog from './AssignSampleBankDialog.svelte';

const options = [
    {
        objectId: 'bank-2',
        name: 'Bank 2',
        memberCount: 12,
        selectedMemberCount: 1,
        movedSampleCount: 2,
        reassignedSampleCount: 1,
        finalMemberCount: 14,
    },
    {
        objectId: 'bank-10',
        name: 'Bank 10',
        memberCount: 127,
        selectedMemberCount: 0,
        movedSampleCount: 3,
        reassignedSampleCount: 0,
        finalMemberCount: 130,
    },
];

describe('AssignSampleBankDialog', () => {
    it('searches existing banks, explains the move, and submits the exact bank', async () => {
        const onsubmit = vi.fn();
        render(AssignSampleBankDialog, {
            props: {
                volumeName: 'Samples',
                sampleCount: 3,
                options,
                blockers: [],
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        const input = screen.getByRole('combobox', { name: 'Sample Bank' });
        await waitFor(() => expect(document.activeElement).toBe(input));
        expect(input.classList).toContain('dialog-field-control');
        await fireEvent.input(input, { target: { value: 'bank 2' } });
        const option = screen.getByRole('option', { name: /Bank 2.*12 members/ });
        expect(screen.getByRole('listbox', { name: 'Sample Banks' }).classList).toContain('dialog-autocomplete-list');
        expect(option.classList).toContain('dialog-autocomplete-option');
        await fireEvent.click(option);

        expect(
            screen.getByText('1 selected Sample is already in this Sample Bank and will remain in place.'),
        ).toBeTruthy();
        expect(
            screen.getByText('1 selected Sample will be detached from its current Sample Bank and appended.'),
        ).toBeTruthy();
        expect(screen.getByText('1 unassigned selected Sample will be linked and appended.')).toBeTruthy();
        expect(screen.getByText('14 of 127 members after assignment')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'Assign to Sample Bank' }));
        expect(onsubmit).toHaveBeenCalledWith('bank-2');
    });

    it('disables over-capacity targets and blocks direct Program assignments', async () => {
        const onsubmit = vi.fn();
        render(AssignSampleBankDialog, {
            props: {
                volumeName: 'Samples',
                sampleCount: 3,
                options,
                blockers: [{ sampleName: 'Direct Sample', programName: '001: Lead' }],
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit,
            },
        });

        const input = screen.getByRole('combobox', { name: 'Sample Bank' });
        await fireEvent.input(input, { target: { value: 'Bank 10' } });
        expect(
            (screen.getByRole('option', { name: /Bank 10.*127 members/ }) as HTMLElement).getAttribute('aria-disabled'),
        ).toBe('true');
        expect(screen.getByText('Direct Sample is assigned directly to Program 001: Lead.')).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Assign to Sample Bank' }) as HTMLButtonElement).disabled).toBe(
            true,
        );
        expect(onsubmit).not.toHaveBeenCalled();
    });
});
