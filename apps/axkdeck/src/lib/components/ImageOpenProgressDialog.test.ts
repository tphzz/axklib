import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ImageOpenProgressDialog from './ImageOpenProgressDialog.svelte';

describe('ImageOpenProgressDialog', () => {
    it('renders determinate job progress and exposes cancellation while opening', async () => {
        const oncancel = vi.fn();
        render(ImageOpenProgressDialog, {
            props: {
                label: 'Resolving sampler objects',
                completed: 2,
                total: 5,
                cancellable: true,
                cancelling: false,
                oncancel,
            },
        });

        expect(screen.getByRole('status').textContent).toContain('Resolving sampler objects');
        const progress = screen.getByRole('progressbar');
        expect(progress.getAttribute('value')).toBe('2');
        expect(progress.getAttribute('max')).toBe('5');
        const cancel = screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement;
        expect(cancel.disabled).toBe(false);

        await fireEvent.click(cancel);
        expect(oncancel).toHaveBeenCalledOnce();
    });

    it('uses indeterminate progress and disables cancellation while preparing the workspace', () => {
        render(ImageOpenProgressDialog, {
            props: {
                label: 'Preparing workspace',
                completed: 5,
                cancellable: false,
                cancelling: false,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('progressbar').hasAttribute('value')).toBe(false);
        const cancel = screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement;
        expect(cancel.disabled).toBe(true);
    });
});
