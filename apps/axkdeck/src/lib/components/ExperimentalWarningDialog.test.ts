import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ExperimentalWarningDialog from './ExperimentalWarningDialog.svelte';

describe('ExperimentalWarningDialog', () => {
    it('requires an explicit acknowledgement and communicates the data-loss risk', async () => {
        const onacknowledge = vi.fn();
        render(ExperimentalWarningDialog, { props: { onacknowledge } });

        const dialog = screen.getByRole('dialog', { name: 'Experimental software' });
        const acknowledge = screen.getByRole('button', { name: 'I understand' });
        const displayedText = dialog.textContent?.replace(/\s+/g, ' ') ?? '';

        expect(displayedText).toContain('may corrupt or destroy disk images');
        expect(displayedText).toContain('Back up disk images before making changes');
        expect(displayedText).toContain('not responsible for data loss');
        expect(screen.queryByRole('button', { name: 'Close' })).toBeNull();
        await waitFor(() => expect(document.activeElement).toBe(acknowledge));

        await fireEvent.keyDown(dialog, { key: 'Escape' });
        await fireEvent.click(dialog.parentElement!);
        expect(onacknowledge).not.toHaveBeenCalled();

        await fireEvent.click(acknowledge);
        expect(onacknowledge).toHaveBeenCalledOnce();
    });
});
