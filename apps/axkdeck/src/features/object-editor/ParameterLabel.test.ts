import { render } from '@testing-library/svelte';
import { describe, expect, it } from 'vitest';
import ParameterLabel from './ParameterLabel.svelte';

describe('parameter label wrappers', () => {
    it('retains the MIDI label text wrapper when no help or comparison is available', () => {
        const view = render(ParameterLabel, {
            label: 'Control 1 Controller',
            parameter: 'controls.1.controller',
        });

        expect(view.container.querySelector('.parameter-label-text')?.textContent).toBe('Control 1 Controller');
        expect(view.queryByRole('button')).toBeNull();
        expect(view.queryByRole('tooltip')).toBeNull();
    });

    it('uses the same label text wrapper when help becomes available', async () => {
        const view = render(ParameterLabel, {
            label: 'Control 1 Controller',
            parameter: 'controls.1.controller',
            description: 'MIDI source assigned to this control.',
        });
        expect(view.container.querySelector('.parameter-label-text')?.textContent).toBe('Control 1 Controller');
        expect(view.getByRole('button', { name: 'Control 1 Controller' })).toBeTruthy();

        await view.rerender({ description: '' });
        expect(view.container.querySelector('.parameter-label-text')?.textContent).toBe('Control 1 Controller');
        expect(view.queryByRole('button')).toBeNull();
    });
});
