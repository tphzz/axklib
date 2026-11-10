import { act, fireEvent, render } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import SourceTabs from './SourceTabs.svelte';

describe('SourceTabs composition', () => {
    it.each(['open', 'create'])('places %s navigation in the shared full-width dialog tab bar', (id) => {
        const view = render(SourceTabs, {
            id,
            label: 'Source kinds',
            value: 'Image',
            available: true,
            onchange: vi.fn(),
        });
        const bar = view.container.querySelector('.source-tabs > .tab-bar');
        expect(bar).not.toBeNull();
        expect(bar?.getAttribute('data-context')).toBe('dialog');
        expect(view.getByRole('tablist').parentElement).toBe(bar);
    });

    it('retains domain IDs, unavailable reasons and native disabling without hiding source kinds', async () => {
        const onchange = vi.fn();
        const view = render(SourceTabs, {
            id: 'open',
            label: 'Source kinds',
            value: 'Image',
            available: false,
            reason: 'Local devices unavailable',
            onchange,
        });
        const tabs = view.getAllByRole('tab');
        expect(tabs).toHaveLength(4);
        expect(tabs[0]!.getAttribute('id')).toBe('open-tab-Image');
        expect(tabs[0]!.getAttribute('aria-controls')).toBe('open-panel-Image');
        for (const tab of tabs.slice(1)) {
            expect(tab.matches(':disabled')).toBe(true);
            expect(tab.getAttribute('aria-describedby')).toBe('open-device-unavailable');
            expect(tab.getAttribute('title')).toBe('Local devices unavailable');
        }
        tabs[0]!.focus();
        await fireEvent.keyDown(tabs[0]!, { key: 'End' });
        await act(() => {});
        expect(onchange).toHaveBeenCalledWith('Image');
        expect(document.activeElement).toBe(tabs[0]);
        await view.rerender({ disabled: true });
        expect(tabs.every((tab) => tab.matches(':disabled'))).toBe(true);
    });

    it('moves focus only after the owner accepts source selection', async () => {
        const onchange = vi.fn((value: 'Image' | 'Device' | 'SCSI2SDv6' | 'SCSI2SD XML') => view.rerender({ value }));
        const view = render(SourceTabs, {
            id: 'create',
            label: 'Destination kinds',
            value: 'Image',
            available: true,
            onchange,
        });
        const image = view.getByRole('tab', { name: 'Image' });
        const xml = view.getByRole('tab', { name: 'SCSI2SD XML' });
        image.focus();
        await fireEvent.keyDown(image, { key: 'End' });
        await act(() => {});
        expect(xml.getAttribute('id')).toBe('create-tab-SCSI2SD-XML');
        expect(xml.getAttribute('aria-controls')).toBe('create-panel-SCSI2SD-XML');
        expect(xml.getAttribute('aria-selected')).toBe('true');
        expect(document.activeElement).toBe(xml);
    });
});
