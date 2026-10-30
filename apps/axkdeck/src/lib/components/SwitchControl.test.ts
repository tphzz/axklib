import { render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import SwitchControl from './SwitchControl.svelte';

describe('SwitchControl', () => {
    it('is controlled by the caller, including pending asynchronous changes', async () => {
        let complete!: () => void;
        const onchange = vi.fn(() => new Promise<void>((resolve) => (complete = resolve)));
        const view = render(SwitchControl, { label: 'Snap', checked: false, onchange });
        const control = screen.getByRole('switch', { name: 'Snap' });
        (control as HTMLButtonElement).click();
        expect(onchange).toHaveBeenCalledExactlyOnceWith(true);
        expect(control.getAttribute('aria-checked')).toBe('false');
        complete();
        await Promise.resolve();
        expect(control.getAttribute('aria-checked')).toBe('false');
        await view.rerender({ label: 'Snap', checked: true, onchange });
        expect(control.getAttribute('aria-checked')).toBe('true');
    });

    it('honors explicit and inherited fieldset disabling', async () => {
        const onchange = vi.fn();
        const fieldset = document.createElement('fieldset');
        fieldset.disabled = true;
        document.body.append(fieldset);
        const view = render(SwitchControl, { target: fieldset, props: { label: 'Follow', checked: true, onchange } });
        const control = screen.getByRole('switch', { name: 'Follow' }) as HTMLButtonElement;
        control.click();
        expect(onchange).not.toHaveBeenCalled();
        fieldset.disabled = false;
        await view.rerender({ label: 'Follow', checked: true, disabled: true, onchange });
        control.click();
        expect(onchange).not.toHaveBeenCalled();
        view.unmount();
        fieldset.remove();
    });

    it('names the diagnostic density and keeps its visible label in the clickable control', () => {
        render(SwitchControl, {
            label: 'Word wrap',
            checked: false,
            density: 'diagnostic',
            showLabel: true,
            onchange: vi.fn(),
        });
        const control = screen.getByRole('switch', { name: 'Word wrap' });
        expect(control.getAttribute('data-density')).toBe('diagnostic');
        expect(control.textContent?.trim()).toBe('Word wrap');
        expect(control.getAttribute('type')).toBe('button');
    });
});
