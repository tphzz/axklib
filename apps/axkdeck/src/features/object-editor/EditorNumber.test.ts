import { render, fireEvent } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import EditorNumber from './EditorNumber.svelte';
describe('editor number controls', () => {
    it('keeps millisecond readouts compact while retaining frame precision', async () => {
        const onchange = vi.fn();
        const view = render(EditorNumber, {
            label: 'Wave end',
            value: 83996,
            min: 0,
            max: 100000,
            scale: 44.1,
            unit: 'ms',
            onchange,
        });
        const input = view.getByRole('spinbutton') as HTMLInputElement;
        expect(input.value).toBe('1904.67');
        await fireEvent.input(input, { target: { value: input.value } });
        expect(onchange).toHaveBeenCalledWith(83996);
    });
    it('shows decimal units while emitting exact native integers', async () => {
        const onchange = vi.fn();
        const view = render(EditorNumber, {
            label: 'Tempo',
            value: 9000,
            min: 8000,
            max: 15999,
            scale: 100,
            unit: 'BPM',
            onchange,
        });
        expect((view.getByRole('spinbutton') as HTMLInputElement).value).toBe('90');
        expect(view.getByRole('slider').getAttribute('max')).toBe('15999');
        await fireEvent.input(view.getByRole('spinbutton'), { target: { value: '120.25' } });
        expect(onchange).toHaveBeenLastCalledWith(12025);
    });
    it('retains invalid text outside the draft until corrected or abandoned', async () => {
        const onchange = vi.fn();
        const oninvalid = vi.fn();
        const view = render(EditorNumber, { label: 'Level', value: 90, onchange, oninvalid });
        await fireEvent.input(view.getByRole('spinbutton'), { target: { value: '' } });
        expect(onchange).not.toHaveBeenCalled();
        expect(oninvalid).toHaveBeenLastCalledWith('Level: enter 0 to 127');
        await fireEvent.blur(view.getByRole('spinbutton'));
        expect((view.getByRole('spinbutton') as HTMLInputElement).value).toBe('90');
        expect(oninvalid).toHaveBeenLastCalledWith('');
    });
    it('restores saved values with both reset gestures', async () => {
        const onchange = vi.fn();
        const view = render(EditorNumber, { label: 'Level', value: 90, resetValue: 100, onchange });
        await fireEvent.doubleClick(view.getByRole('slider'));
        expect(onchange).toHaveBeenLastCalledWith(100);
        await fireEvent.keyDown(view.getByRole('slider'), { key: 'Backspace', altKey: true });
        expect(onchange).toHaveBeenCalledTimes(2);
    });
});
