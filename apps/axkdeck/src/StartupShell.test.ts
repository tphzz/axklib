import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import StartupShell from './StartupShell.svelte';

describe('StartupShell', () => {
    it('paints a stable starting state while retaining the startup warning', () => {
        const { container } = render(StartupShell, {
            props: { onacknowledge: vi.fn(), onretry: vi.fn(), onopensettings: vi.fn() },
        });
        const brand = container.querySelector('.startup-brand');
        expect(brand?.textContent).toBe('axkdeck');
        expect(brand?.children).toHaveLength(1);
        expect(brand?.querySelector('span, svg, img, button')).toBeNull();
        expect(screen.getAllByText('Starting services...')).toHaveLength(2);
        const inspector = screen.getByRole('complementary', { name: 'Object inspector' });
        expect(inspector.querySelector('.panel-heading')).toBeTruthy();
        expect(screen.getByRole('heading', { name: 'Object details', level: 2 })).toBeTruthy();
        expect(screen.getByRole('dialog', { name: 'Experimental software' })).toBeTruthy();
    });

    it('offers recovery without leaving a blank window', async () => {
        const onretry = vi.fn();
        const onopensettings = vi.fn();
        render(StartupShell, {
            props: {
                status: 'unavailable',
                message: 'Connection timed out',
                warningOpen: false,
                onacknowledge: vi.fn(),
                onretry,
                onopensettings,
            },
        });
        expect(screen.getByRole('alert').textContent).toContain('Connection timed out');
        const views = screen.getByRole('navigation', { name: 'Object views' });
        expect(views.getAttribute('data-appearance')).toBe('workspace');
        expect(views.querySelectorAll('button')).toHaveLength(5);
        for (const button of views.querySelectorAll('button')) {
            expect(button.disabled).toBe(true);
            expect(button.getAttribute('aria-pressed')).toBe('false');
            expect(button.getAttribute('aria-label')).toBe(button.textContent?.trim());
        }
        await fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
        await fireEvent.click(screen.getByRole('button', { name: 'Connection settings' }));
        expect(onretry).toHaveBeenCalledOnce();
        expect(onopensettings).toHaveBeenCalledOnce();
    });
});
