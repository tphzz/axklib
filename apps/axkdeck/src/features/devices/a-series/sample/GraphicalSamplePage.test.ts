import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import { fireEvent, render } from '@testing-library/svelte';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { flushSync } from 'svelte';
import GraphicalSamplePage from './GraphicalSamplePage.svelte';
import { sampleTabs } from './fields';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';

beforeEach(() => {
    vi.stubGlobal(
        'ResizeObserver',
        class {
            observe() {}
            disconnect() {}
        },
    );
    vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
});

function setup(id: string, blockedParameters: string[] = []) {
    const page = sampleTabs.flatMap((tab) => tab.pages).find((item) => item.id === id)!;
    const draft = new EditorDraft(
        Object.fromEntries(
            page.fields.map((field) => [
                field.key,
                field.boolean ? false : (field.options?.[0]?.value ?? Math.max(0, field.min)),
            ]),
        ),
    );
    const document = {
        preferencesScope: {},
        draft,
        detail: {
            editing: {
                blockedParameters,
                blockedParameterReasons: Object.fromEntries(
                    blockedParameters.map((key) => [key, 'The stereo channels have different stored values.']),
                ),
                ...sampleFormatFixture(),
                unavailableParameters: {},
                eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
            },
        },
        inputErrors: {},
    } as unknown as ObjectEditorDocument;
    return { draft, document, view: render(GraphicalSamplePage, { document, page, disabled: false }) };
}

describe('graphical Sample controls', () => {
    it('has one EQ handle and groups width wheel edits with one undo', async () => {
        const { draft, view } = setup('sample-eq');
        expect(view.container.querySelectorAll('[data-handle]').length).toBe(1);
        const handle = view.container.querySelector('[data-handle]')!;
        await fireEvent.wheel(handle, { deltaY: -1 });
        await fireEvent.wheel(handle, { deltaY: -1 });
        expect(draft.changes).toEqual({ sample_eq_width_tenths: 20 });
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
        await fireEvent.keyDown(handle, { key: 'ArrowUp', altKey: true });
        await fireEvent.keyUp(handle, { key: 'ArrowUp', altKey: true });
        expect(draft.changes).toEqual({ sample_eq_width_tenths: 15 });
    });
    it('does not intercept shelf or blocked width wheel gestures', async () => {
        const { draft, view } = setup('sample-eq', ['sample_eq_width_tenths']);
        const handle = view.container.querySelector('[data-handle]')!;
        const wheel = new WheelEvent('wheel', { deltaY: -1, cancelable: true });
        handle.dispatchEvent(wheel);
        expect(wheel.defaultPrevented).toBe(false);
        expect(draft.dirty).toBe(false);
        flushSync(() => draft.set('sample_eq_type', 1));
        await fireEvent.keyDown(handle, { key: 'ArrowUp', altKey: true });
        await fireEvent.keyUp(handle, { key: 'ArrowUp', altKey: true });
        expect(draft.changes).toEqual({ sample_eq_type: 1 });
    });
    it('does not refit an envelope on release and fits only on explicit request', async () => {
        const { draft, view } = setup('aeg');
        const handle = view.getByRole('button', { name: /^Peak:/ });
        const path = () => view.container.querySelector('[data-trace="envelope"]')!.getAttribute('d');
        await fireEvent.keyDown(handle, { key: 'ArrowLeft', shiftKey: true });
        const dragging = path();
        await fireEvent.keyUp(handle, { key: 'ArrowLeft' });
        expect(path()).toBe(dragging);
        const changes = { ...draft.changes };
        await fireEvent.click(view.getByRole('button', { name: 'Fit envelope to width' }));
        expect(path()).not.toBe(dragging);
        expect(draft.changes).toEqual(changes);
    });
    it('does not edit a scaling pair with an unavailable boundary', () => {
        const { draft, view } = setup('level-scaling');
        flushSync(() => {
            const { level_scaling_break2: _missing, ...values } = draft.values;
            draft.storedValues = values;
        });
        expect((view.getByRole('button', { name: /^Point 1:/ }) as HTMLButtonElement).disabled).toBe(true);
        expect((view.getByRole('button', { name: /^Point 2:/ }) as HTMLButtonElement).disabled).toBe(true);
    });
    it('synchronizes envelope keyboard handles and numeric fields with one undo', async () => {
        const { draft, view } = setup('feg');
        const handle = view.getByRole('button', { name: /^Initial:/ });
        await fireEvent.focus(handle);
        await fireEvent.keyDown(handle, { key: 'ArrowUp' });
        await fireEvent.keyUp(handle, { key: 'ArrowUp' });
        expect(draft.values['feg.init_level']).toBe(1);
        expect((view.getByRole('spinbutton', { name: 'Init level' }) as HTMLInputElement).value).toBe('1');
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
        await fireEvent.input(view.getByRole('spinbutton', { name: 'Release level' }), { target: { value: '-64' } });
        expect(view.getByRole('button', { name: /^Release: -64,/ })).toBeTruthy();
    });
    it('blocks only the corresponding handle and preserves other envelope edits', async () => {
        const { draft, view } = setup('peg', ['peg.init_level']);
        expect((view.getByRole('button', { name: /^Initial:/ }) as HTMLButtonElement).disabled).toBe(true);
        const attack = view.getByRole('button', { name: /^Attack:/ });
        expect((attack as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.keyDown(attack, { key: 'ArrowDown' });
        await fireEvent.keyUp(attack, { key: 'ArrowDown' });
        expect(draft.changes).toEqual({ 'peg.attack_level': -1 });
    });
    it('updates LFO depth and inversion without mutating on destination selection', async () => {
        const { draft, view } = setup('lfo');
        const path = () => view.container.querySelector('.modulation')!.getAttribute('d');
        const zero = path();
        expect(view.container.querySelector('.reference')!.getAttribute('d')).not.toBe(zero);
        await fireEvent.input(view.getByRole('spinbutton', { name: 'Pitch depth' }), { target: { value: '90' } });
        expect(path()).not.toBe(zero);
        const normal = path();
        await fireEvent.click(view.getByRole('switch', { name: 'Invert pitch phase' }));
        expect(path()).not.toBe(normal);
        flushSync(() => draft.discard());
        await fireEvent.click(view.getByRole('button', { name: 'Show Cutoff modulation' }));
        expect(draft.dirty).toBe(false);
    });
    it('edits only a rate from horizontal envelope keys, with one undo', async () => {
        const { draft, view } = setup('aeg');
        const handle = view.getByRole('button', { name: /^Peak:/ });
        await fireEvent.keyDown(handle, { key: 'ArrowLeft' });
        await fireEvent.keyUp(handle, { key: 'ArrowLeft' });
        expect(draft.changes).toEqual({ 'aeg.attack_rate': 1 });
        flushSync(() => draft.undo());
        expect(draft.dirty).toBe(false);
    });
    it('keeps stored sample speed intact when Sample & Hold delegates it to the Program', async () => {
        const { draft, view } = setup('lfo');
        await fireEvent.click(view.getByRole('button', { name: 'Wave: S & H' }));
        expect((view.getByRole('textbox', { name: 'Speed' }) as HTMLInputElement).value).toBe('Program');
        expect(draft.changes).toEqual({ 'lfo.wave': 3 });
        await fireEvent.click(view.getByRole('button', { name: 'Wave: Saw' }));
        expect((view.getByRole('spinbutton', { name: 'Speed' }) as HTMLInputElement).value).toBe('1');
    });
    it('supports endpoint rate bounds and keeps the initial level handle vertical-only', async () => {
        const { draft, view } = setup('aeg');
        const rate = view.getByRole('button', { name: /^Release:/ });
        await fireEvent.keyDown(rate, { key: 'End' });
        await fireEvent.keyUp(rate, { key: 'End' });
        expect(draft.changes).toEqual({ 'aeg.release_rate': 127 });
        await fireEvent.keyDown(rate, { key: 'Home' });
        await fireEvent.keyUp(rate, { key: 'Home' });
        expect(draft.dirty).toBe(false);
        view.unmount();
        const { draft: filterDraft, view: filterView } = setup('feg');
        const level = filterView.getByRole('button', { name: /^Initial:/ });
        await fireEvent.keyDown(level, { key: 'ArrowLeft' });
        await fireEvent.keyUp(level, { key: 'ArrowLeft' });
        expect(filterDraft.dirty).toBe(false);
    });
    it('uses compact menus for random filter response modes and preserves numeric validation', async () => {
        const { document, draft, view } = setup('filter-scaling');
        expect(view.getByRole('button', { name: 'Velocity to cutoff mode' }).getAttribute('aria-haspopup')).toBe(
            'listbox',
        );
        const input = view.getByRole('spinbutton', { name: 'Velocity to cutoff' });
        await fireEvent.input(input, { target: { value: '' } });
        expect(document.inputErrors.filter_velocity_to_cutoff).toBeTruthy();
        expect(draft.dirty).toBe(false);
        await fireEvent.blur(input);
        expect(document.inputErrors.filter_velocity_to_cutoff).toBe('');
    });
});
