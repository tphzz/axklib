import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { createRawSnippet } from 'svelte';
import { describe, expect, it, vi } from 'vitest';
import ImportSourceHeader from './ImportSourceHeader.svelte';

describe('import source chips', () => {
    it('does not steal focus moved while async removal was pending', async () => {
        let finish!: () => void;
        const view = render(ImportSourceHeader, {
            label: 'TX16W disk set',
            summary: '2 disks',
            members: [
                { id: 1, name: 'first.img' },
                { id: 2, name: 'second.img' },
            ],
            onremove: async () => {
                await new Promise<void>((resolve) => {
                    finish = resolve;
                });
                await view.rerender({ members: [{ id: 2, name: 'second.img' }] });
            },
            actions: createRawSnippet(() => ({ render: () => '<button type="button">Add disks</button>' })),
        });
        const remove = screen.getByRole('button', { name: 'Remove first.img' });
        remove.focus();
        await fireEvent.click(remove);
        const add = screen.getByRole('button', { name: 'Add disks' });
        add.focus();
        finish();
        await waitFor(() => expect(screen.queryByRole('button', { name: 'Remove first.img' })).toBeNull());
        await new Promise((resolve) => setTimeout(resolve, 0));
        expect(document.activeElement).toBe(add);
    });

    it('shows automatic origins and preserves complete names for keyboard help', async () => {
        const name = 'A very long original library disk filename.img';
        render(ImportSourceHeader, {
            label: 'TX16W disk set',
            summary: '2 disks',
            members: [
                { id: 1, name },
                { id: 2, name: 'second.img', autoAdded: true },
            ],
            onremove: vi.fn(),
        });
        expect(screen.getByText('Auto-added')).toBeTruthy();
        await fireEvent.focus(screen.getByRole('button', { name }));
        expect(screen.getByRole('tooltip').textContent).toBe(name);
        expect(screen.getByRole('button', { name: `Remove ${name}` })).toBeTruthy();
    });

    it('restores removal focus to the next chip, previous chip, then Add disks', async () => {
        let members = [
            { id: 1, name: 'first.img' },
            { id: 2, name: 'second.img' },
            { id: 3, name: 'third.img' },
        ];
        const onremove = async (id: number) => {
            members = members.filter((member) => member.id !== id);
            await view.rerender({ members });
        };
        const view = render(ImportSourceHeader, {
            label: 'TX16W disk set',
            summary: '3 disks',
            members,
            onremove,
            actions: createRawSnippet(() => ({ render: () => '<button type="button">Add disks</button>' })),
        });
        const second = screen.getByRole('button', { name: 'Remove second.img' });
        second.focus();
        await fireEvent.click(second);
        await waitFor(() =>
            expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Remove third.img' })),
        );
        await fireEvent.click(document.activeElement!);
        await waitFor(() =>
            expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Remove first.img' })),
        );
        await fireEvent.click(document.activeElement!);
        await waitFor(() => expect(document.activeElement).toBe(screen.getByRole('button', { name: 'Add disks' })));
    });
});
