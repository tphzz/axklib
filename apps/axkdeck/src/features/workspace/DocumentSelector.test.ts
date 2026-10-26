import { selectValue } from '../../test/selectControl';
import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import DocumentSelector from './DocumentSelector.svelte';
import { nativeWorkspaceFixture } from '../../test/nativeWorkspaceFixture';

describe('Document selector', () => {
    it('restores the active document when a requested switch is declined', async () => {
        const fixture = nativeWorkspaceFixture();
        const onselect = vi.fn().mockResolvedValue(undefined);
        render(DocumentSelector, {
            documents: [...fixture.documents, { ...fixture.documents[0], id: 'other' }],
            selected: fixture.documentId,
            disabled: false,
            discovery: fixture.discovery,
            onselect,
            onresolve: vi.fn(),
        });
        const control = screen.getByRole('combobox') as HTMLButtonElement;
        await selectValue(control, 'other');
        await waitFor(() => expect(control.value).toBe(fixture.documentId));
        expect(onselect).toHaveBeenCalledWith('other');
    });

    it('retains incomplete candidates and exposes invalid discovery without offering them as choices', async () => {
        const onselect = vi.fn().mockResolvedValue(undefined);
        const onresolve = vi.fn().mockResolvedValue(undefined);
        render(DocumentSelector, {
            documents: [
                ...nativeWorkspaceFixture().documents,
                { ...nativeWorkspaceFixture().documents[0], id: 'other' },
            ],
            selected: null,
            disabled: false,
            discovery: {
                selectionRequired: true,
                candidates: [
                    {
                        formatId: 'yamaha.tx16w',
                        state: 'INCOMPLETE',
                        sourceMemberIds: ['source-1'],
                        diagnostics: ['Missing wave companion'],
                    },
                    {
                        formatId: 'invalid-format',
                        state: 'INVALID',
                        sourceMemberIds: ['source-1'],
                        diagnostics: ['Invalid header'],
                    },
                ],
            },
            onselect,
            onresolve,
        });
        await selectValue(screen.getByRole('combobox'), 'document-a-series-source-1');
        expect(onselect).toHaveBeenCalledWith('document-a-series-source-1');
        await fireEvent.click(screen.getByText('Choose native formats'));
        expect(screen.getAllByRole('checkbox')).toHaveLength(1);
        await fireEvent.click(screen.getByRole('checkbox'));
        await fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
        expect(onresolve).toHaveBeenCalledWith(['yamaha.tx16w']);
        await fireEvent.click(screen.getByText('Discovery notices (2)'));
        expect(screen.getByText('Invalid header')).toBeTruthy();
        const details = screen.getByText('Invalid header').closest('details')!;
        await fireEvent.keyDown(details, { key: 'Escape' });
        expect(details.open).toBe(false);
    });
});
