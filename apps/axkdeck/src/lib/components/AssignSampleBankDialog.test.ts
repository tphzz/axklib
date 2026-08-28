/// <reference types="node" />

import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

import { fireEvent, render, screen, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import AssignSampleBankDialog from './AssignSampleBankDialog.svelte';

const appStyles = readFileSync(resolve(process.cwd(), 'src/app.css'), 'utf8');
const dialogSource = readFileSync(resolve(process.cwd(), 'src/lib/components/AssignSampleBankDialog.svelte'), 'utf8');

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
    {
        objectId: 'bank-current',
        name: 'Current Bank',
        memberCount: 3,
        selectedMemberCount: 3,
        movedSampleCount: 0,
        reassignedSampleCount: 0,
        finalMemberCount: 3,
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
        const status = screen.getByRole('status');
        await waitFor(() => expect(document.activeElement).toBe(input));
        expect(input.classList).toContain('dialog-field-control');
        expect(input.getAttribute('aria-expanded')).toBe('false');
        expect(screen.queryByRole('listbox', { name: 'Sample Banks' })).toBeNull();
        expect(status.textContent).toBe('No Sample Bank selected');

        await fireEvent.click(input);
        expect(input.getAttribute('aria-expanded')).toBe('true');
        expect(screen.getByRole('dialog', { name: 'Assign to Sample Bank' }).classList).toContain(
            'dialog-popovers-visible',
        );

        const listbox = screen.getByRole('listbox', { name: 'Sample Banks' });
        const unfilteredOption = screen.getByRole('option', { name: /Bank 2.*12 members/ });
        expect(listbox.classList).toContain('dialog-autocomplete-list');
        expect(unfilteredOption.classList).toContain('dialog-autocomplete-option');

        const optionRule = appStyles.match(/\.dialog-autocomplete-option\s*\{[^}]+\}/)?.[0];
        const popupRule = dialogSource.match(/\.sample-bank-options\s*\{[^}]+\}/)?.[0];
        const localOptionRule = dialogSource.match(
            /\.sample-bank-options\s+:global\(\.dialog-autocomplete-option\)\s*\{[^}]+\}/,
        )?.[0];
        if (!optionRule || !popupRule || !localOptionRule) throw new Error('Autocomplete visual rules are missing');
        expect(popupRule).toContain('position: absolute');
        expect(localOptionRule).not.toMatch(/min-height|padding-top|padding-bottom/);

        const style = document.createElement('style');
        style.textContent = optionRule.replaceAll('var(--dialog-control-font-size)', '11px');
        document.head.append(style);

        const optionStyle = getComputedStyle(unfilteredOption);
        expect(optionStyle.minHeight).toBe('28px');
        expect(optionStyle.paddingTop).toBe('0px');
        expect(optionStyle.paddingBottom).toBe('0px');
        expect(optionStyle.fontSize).toBe('11px');
        style.remove();

        await fireEvent.input(input, { target: { value: 'bank 2' } });
        const option = screen.getByRole('option', { name: /Bank 2.*12 members/ });
        expect(option.classList).toContain('dialog-autocomplete-option');
        await fireEvent.click(option);
        expect(input.getAttribute('aria-expanded')).toBe('false');
        expect(screen.queryByRole('listbox', { name: 'Sample Banks' })).toBeNull();

        expect(screen.getByRole('status')).toBe(status);
        expect(status.textContent).toBe('1 already here · 1 moved from another bank · 1 linked · 14 of 127 members');
        const warningSegment = Array.from(status.querySelectorAll('span')).find(
            (segment) => segment.textContent?.trim() === '· 1 moved from another bank',
        );
        expect(warningSegment?.classList).toContain('dialog-warning');

        const statusRule = dialogSource.match(/\.assignment-status\s*\{[^}]+\}/)?.[0];
        if (!statusRule) throw new Error('Assignment status visual rule is missing');
        expect(statusRule).toContain('height: 16px');
        expect(statusRule).toContain('overflow: hidden');
        expect(statusRule).toContain('text-overflow: ellipsis');
        expect(statusRule).toContain('white-space: nowrap');

        await fireEvent.click(input);
        await fireEvent.pointerDown(screen.getByRole('button', { name: 'Cancel' }));
        expect((input as HTMLInputElement).value).toBe('Bank 2');
        expect(input.getAttribute('aria-expanded')).toBe('false');
        expect(status.textContent).toContain('14 of 127 members');

        await fireEvent.click(screen.getByRole('button', { name: 'Assign to Sample Bank' }));
        expect(onsubmit).toHaveBeenCalledWith('bank-2');
    });

    it('does not open on focus and supports explicit keyboard opening and dismissal', async () => {
        render(AssignSampleBankDialog, {
            props: {
                volumeName: 'Samples',
                sampleCount: 1,
                options,
                blockers: [],
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        const input = screen.getByRole('combobox', { name: 'Sample Bank' });
        await waitFor(() => expect(document.activeElement).toBe(input));
        await fireEvent.blur(input);
        await fireEvent.focus(input);
        expect(screen.queryByRole('listbox', { name: 'Sample Banks' })).toBeNull();

        await fireEvent.keyDown(input, { key: 'ArrowDown' });
        expect(screen.getByRole('listbox', { name: 'Sample Banks' })).toBeTruthy();
        expect(input.getAttribute('aria-activedescendant')).toBe('sample-bank-option-bank-2');

        await fireEvent.keyDown(input, { key: 'Escape' });
        expect(input.getAttribute('aria-expanded')).toBe('false');
        expect(screen.queryByRole('listbox', { name: 'Sample Banks' })).toBeNull();
    });

    it('clears the query and selected bank in one click, then reopens all options', async () => {
        render(AssignSampleBankDialog, {
            props: {
                volumeName: 'Samples',
                sampleCount: 3,
                options,
                blockers: [],
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        const input = screen.getByRole('combobox', { name: 'Sample Bank' }) as HTMLInputElement;
        const status = screen.getByRole('status');
        expect(screen.queryByRole('button', { name: 'Clear Sample Bank' })).toBeNull();

        await fireEvent.input(input, { target: { value: 'Bank 2' } });
        const clear = screen.getByRole('button', { name: 'Clear Sample Bank' });
        expect(clear.classList).toContain('dialog-autocomplete-clear');
        await fireEvent.click(screen.getByRole('option', { name: /Bank 2.*12 members/ }));
        expect(screen.getByRole('status')).toBe(status);
        expect(status.textContent).toContain('14 of 127 members');
        expect((screen.getByRole('button', { name: 'Assign to Sample Bank' }) as HTMLButtonElement).disabled).toBe(
            false,
        );

        await fireEvent.click(clear);

        expect(input.value).toBe('');
        expect(screen.queryByRole('button', { name: 'Clear Sample Bank' })).toBeNull();
        expect(screen.getByRole('status')).toBe(status);
        expect(status.textContent).toBe('No Sample Bank selected');
        expect((screen.getByRole('button', { name: 'Assign to Sample Bank' }) as HTMLButtonElement).disabled).toBe(
            true,
        );
        await waitFor(() => expect(document.activeElement).toBe(input));
        expect(input.getAttribute('aria-expanded')).toBe('true');
        const listbox = screen.getByRole('listbox', { name: 'Sample Banks' });
        expect(within(listbox).getAllByRole('option')).toHaveLength(3);

        await fireEvent.pointerDown(within(listbox).getAllByRole('option')[0]);
        expect(screen.getByRole('listbox', { name: 'Sample Banks' })).toBeTruthy();

        await fireEvent.pointerDown(screen.getByRole('button', { name: 'Cancel' }));
        expect(input.value).toBe('');
        expect(input.getAttribute('aria-expanded')).toBe('false');
        expect(screen.queryByRole('listbox', { name: 'Sample Banks' })).toBeNull();
    });

    it('summarizes a target that already contains every selected Sample without resizing', async () => {
        render(AssignSampleBankDialog, {
            props: {
                volumeName: 'Samples',
                sampleCount: 3,
                options,
                blockers: [],
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        const input = screen.getByRole('combobox', { name: 'Sample Bank' });
        const status = screen.getByRole('status');
        await fireEvent.input(input, { target: { value: 'Current Bank' } });
        await fireEvent.click(screen.getByRole('option', { name: /Current Bank.*3 members/ }));

        expect(screen.getByRole('status')).toBe(status);
        expect(status.textContent).toBe('No changes · 3 of 127 members');
        expect((screen.getByRole('button', { name: 'Assign to Sample Bank' }) as HTMLButtonElement).disabled).toBe(
            true,
        );
    });

    it('disables over-capacity targets', async () => {
        render(AssignSampleBankDialog, {
            props: {
                volumeName: 'Samples',
                sampleCount: 3,
                options,
                blockers: [],
                busy: false,
                error: '',
                oncancel: vi.fn(),
                onsubmit: vi.fn(),
            },
        });

        const input = screen.getByRole('combobox', { name: 'Sample Bank' });
        await fireEvent.input(input, { target: { value: 'Bank 10' } });
        expect(
            (screen.getByRole('option', { name: /Bank 10.*127 members/ }) as HTMLElement).getAttribute('aria-disabled'),
        ).toBe('true');
    });

    it('disables Sample Bank selection when direct Program assignments block the operation', async () => {
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

        const input = screen.getByRole('combobox', { name: 'Sample Bank' }) as HTMLInputElement;
        expect(input.disabled).toBe(true);
        await fireEvent.click(input);
        expect(screen.queryByRole('listbox', { name: 'Sample Banks' })).toBeNull();
        expect(screen.getByRole('status').textContent).toBe('Sample Bank selection unavailable');
        expect(screen.getByText('Direct Sample is assigned directly to Program 001: Lead.')).toBeTruthy();
        expect((screen.getByRole('button', { name: 'Assign to Sample Bank' }) as HTMLButtonElement).disabled).toBe(
            true,
        );
        expect(onsubmit).not.toHaveBeenCalled();
    });
});
