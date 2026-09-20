import { fireEvent, render, waitFor, within } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import SampleEditorHarness from '../../../../test/SampleEditorHarness.svelte';

beforeEach(() => {
    Element.prototype.scrollIntoView = vi.fn();
    window.history.replaceState({}, '', '/');
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
});

type Queries = ReturnType<typeof within>;

async function choose(scope: Queries, label: RegExp, option: string) {
    const group = scope.queryByRole('group', { name: label });
    if (group) {
        await fireEvent.click(within(group).getByRole('button', { name: new RegExp(`: ${option}$`, 'i') }));
        return;
    }
    await fireEvent.click(scope.getByRole('button', { name: label }));
    await fireEvent.click(within(document.body).getByRole('option', { name: option }));
}

describe('Hardware-aligned Sample editor workflow', () => {
    it('edits the saved Loop Mode after Loop End in Waveform, not from the transport', async () => {
        const view = render(SampleEditorHarness);
        const waveform = await view.findByRole('region', { name: 'Waveform editor' });
        const fields = within(waveform);
        const mode =
            fields.queryByRole('group', { name: /^loop mode$/i }) ??
            fields.getByRole('button', { name: /^loop mode$/i });
        const loopEnd = fields.getByRole('spinbutton', { name: 'Loop end' });
        expect(loopEnd.compareDocumentPosition(mode) & Node.DOCUMENT_POSITION_FOLLOWING).not.toBe(0);
        const transport = within(view.container.querySelector('.sample-transport')! as HTMLElement);
        expect(transport.queryByRole('group', { name: /^(loop mode|playback)$/i })).toBeNull();
        expect(transport.queryByRole('button', { name: /^(loop mode|playback)(:|$)/i })).toBeNull();
        expect((view.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(true);

        await choose(fields, /^loop mode$/i, 'Reverse');
        expect((view.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(view.getByRole('button', { name: 'Save' }));
        await waitFor(() => expect(view.getByLabelText('Write count').textContent).toBe('1'));
        await waitFor(() =>
            expect((view.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(true),
        );
        expect(view.getByRole('button', { name: 'Waveform' }).getAttribute('aria-pressed')).toBe('true');
    });

    it('separates saved sample parameters from local display, audition and calculation helpers', async () => {
        const view = render(SampleEditorHarness);
        await fireEvent.click(await view.findByRole('button', { name: 'Sample Info' }));
        const panel = within(view.getByRole('tabpanel'));
        expect(panel.getByRole('heading', { name: 'Sample parameters' })).toBeTruthy();
        expect(panel.getByRole('heading', { name: 'Display & audition' })).toBeTruthy();
        expect(panel.getByRole('heading', { name: 'Tools' })).toBeTruthy();
        expect(panel.getByRole('button', { name: /end type: address/i }).getAttribute('aria-pressed')).toBe('true');

        await choose(panel, /^end type$/i, 'Length');
        await fireEvent.input(panel.getByRole('spinbutton', { name: /loop monitor lead-in/i }), {
            target: { value: '-125' },
        });
        await choose(panel, /^loop beat count$/i, '4');
        expect((view.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(true);
        expect(view.getByLabelText('Write count').textContent).toBe('0');

        await fireEvent.click(view.getByRole('button', { name: 'Select B' }));
        await view.findByText('Sample B', { selector: 'strong' });
        const second = within(view.getByRole('tabpanel'));
        expect(view.getByRole('button', { name: 'Sample Info' }).getAttribute('aria-pressed')).toBe('true');
        expect(second.getByRole('button', { name: /end type: length/i }).getAttribute('aria-pressed')).toBe('true');
        expect((second.getByRole('spinbutton', { name: /loop monitor lead-in/i }) as HTMLInputElement).value).toBe(
            '-125',
        );
        expect((view.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(true);
    });

    it('saves Loop Tempo and Wave Start Velocity Sensitivity while retaining Sample Info', async () => {
        const view = render(SampleEditorHarness);
        await fireEvent.click(await view.findByRole('button', { name: 'Sample Info' }));
        const panel = within(view.getByRole('tabpanel'));
        await fireEvent.input(panel.getByRole('spinbutton', { name: /^loop tempo$/i }), {
            target: { value: '130.25' },
        });
        await fireEvent.input(panel.getByRole('spinbutton', { name: /^wave start velocity sensitivity$/i }), {
            target: { value: '-12' },
        });
        expect((view.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(view.getByRole('button', { name: 'Save' }));
        await waitFor(() => expect(view.getByLabelText('Write count').textContent).toBe('1'));
        await waitFor(() =>
            expect((view.getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(true),
        );
        expect(view.getByRole('button', { name: 'Sample Info' }).getAttribute('aria-pressed')).toBe('true');
        expect((panel.getByRole('spinbutton', { name: /^loop tempo$/i }) as HTMLInputElement).value).toBe('130.25');
        expect(
            (panel.getByRole('spinbutton', { name: /^wave start velocity sensitivity$/i }) as HTMLInputElement).value,
        ).toBe('-12');
    });
});
