import { act, fireEvent, render } from '@testing-library/svelte';
import { afterAll, beforeAll, describe, expect, it, vi } from 'vitest';
import { programEditorFixture } from '../../../../test/programEditorFixture';
import { EditorNavigation } from '../../../object-editor/navigation.svelte';
import ProgramEditor from './ProgramEditor.svelte';
import { ProgramDraft } from './draft.svelte';

const originalScrollIntoView = Element.prototype.scrollIntoView;
beforeAll(() => {
    Element.prototype.scrollIntoView = vi.fn();
});
afterAll(() => {
    Element.prototype.scrollIntoView = originalScrollIntoView;
});

function setup(tab = 'setup', native = false, editable = true, inactive = false) {
    const fixture = programEditorFixture(native, editable);
    const navigation = new EditorNavigation();
    navigation.tab = tab;
    const view = render(ProgramEditor, {
        document: fixture.document,
        navigation,
        panelId: 'program-test-panel',
        inactive,
    });
    return { ...fixture, navigation, view };
}

describe('Program editor controls', () => {
    it('uses note labels and the shared velocity range editor in Program Key/Velocity', async () => {
        const { view, document } = setup('easy-edit');
        await fireEvent.click(view.getByRole('button', { name: 'Key/Velocity' }));
        expect(view.getByRole('spinbutton', { name: 'Low key' }).parentElement?.textContent).toContain('C-2');
        expect(view.getByRole('spinbutton', { name: 'High key' }).parentElement?.textContent).toContain('G8');
        await fireEvent.keyDown(view.getByRole('slider', { name: 'High velocity boundary' }), { key: 'ArrowDown' });
        await fireEvent.keyUp(view.getByRole('slider', { name: 'High velocity boundary' }), { key: 'ArrowDown' });
        expect(document.draft.changes).toEqual({ 'assignments.0.velocity_high': 126 });
    });
    it('does not badge shared algorithm or Ef4 generic controls as new parameters', async () => {
        const fixture = programEditorFixture();
        fixture.catalog.formats.push(programEditorFixture(true).format);
        const navigation = new EditorNavigation();
        navigation.tab = 'effects';
        const view = render(ProgramEditor, { document: fixture.document, navigation, panelId: 'effect-markers' });
        expect(
            view
                .getByRole('spinbutton', { name: 'Parameter 1' })
                .closest('.parameter-field')
                ?.querySelector('.extended-parameter'),
        ).toBeNull();
        await fireEvent.click(view.getByRole('button', { name: 'Select Ef4' }));
        expect(
            view
                .getByRole('spinbutton', { name: 'Input level' })
                .closest('.parameter-field')
                ?.querySelector('.extended-parameter'),
        ).toBeNull();
        expect(view.container.querySelector('.effect-heading .extended-parameter')).not.toBeNull();
    });
    it('bounds assignment rendering and reaches offscreen targets through keyboard navigation', async () => {
        const fixture = programEditorFixture();
        fixture.editing.targets.push(
            ...Array.from({ length: 2048 }, (_, index) => ({
                ...fixture.editing.targets[1]!,
                objectId: `target-${index}`,
                name: `Target ${index}`,
            })),
        );
        const navigation = new EditorNavigation();
        navigation.tab = 'sample-select';
        const view = render(ProgramEditor, { document: fixture.document, navigation, panelId: 'large-assignments' });
        await fireEvent.click(view.getByRole('checkbox', { name: 'Show only assigned' }));
        expect(view.container.querySelectorAll('tbody tr[data-row-index]').length).toBeGreaterThan(0);
        expect(view.container.querySelectorAll('tbody tr').length).toBeLessThan(60);
        await fireEvent.keyDown(view.getAllByRole('button', { name: /^Duplicate Sample/ })[0]!, { key: 'End' });
        expect(view.getByRole('button', { name: 'Target 2047 Sample' })).toBe(document.activeElement);
        await fireEvent.click(view.getByRole('button', { name: 'Receive channel assign: Target 2047' }));
        await fireEvent.click(view.getByRole('option', { name: '=Sample' }));
        expect((fixture.document.draft as ProgramDraft).assignments.at(-1)?.name).toBe('Target 2047');
    });
    it('keeps the keyboard when Program limits clip all keys or velocities', async () => {
        const { document, view } = setup('easy-edit');
        await act(() => document.draft.patch({ 'assignments.0.key_high': 24, 'assignments.1.velocity_high': 0 }));
        await fireEvent.click(view.getByRole('button', { name: 'Key/Velocity' }));
        expect(view.getByRole('group', { name: /^Keyboard/ })).toBeTruthy();
        expect(view.queryAllByRole('button', { name: /^Select mapping/ })).toHaveLength(0);
        expect(view.queryByText('No resolved playable range for this assignment.')).toBeNull();
    });

    it('prevents unknown assignment edits through mapping and envelope handles', async () => {
        const fixture = programEditorFixture();
        fixture.editing.assignments[0]!.kind = 'UNKNOWN';
        fixture.document.draft = new ProgramDraft(fixture.editing.values, fixture.editing.assignments);
        const navigation = new EditorNavigation();
        navigation.tab = 'easy-edit';
        const view = render(ProgramEditor, { document: fixture.document, navigation, panelId: 'unknown-test' });
        await fireEvent.click(view.getByRole('button', { name: 'Key/Velocity' }));
        const key = view.getByRole('button', { name: 'Low key limit' });
        expect(key).toHaveProperty('disabled', true);
        await fireEvent.keyDown(key, { key: 'ArrowRight' });
        await fireEvent.click(view.getByRole('button', { name: 'Amp EG' }));
        for (const handle of view.queryAllByRole('button', { name: /^Peak:/ })) {
            await fireEvent.keyDown(handle, { key: 'ArrowLeft' });
        }
        expect(fixture.document.draft.dirty).toBe(false);
    });

    it('ignores removed row input errors and restores them on undo', async () => {
        const { document } = setup();
        document.inputErrors['assignments.0.level_offset'] = 'Enter a valid offset';
        await act(() => (document.draft as ProgramDraft).removeAssignment(0));
        expect(document.canSave).toBe(true);
        await act(() => document.draft.undo());
        expect(document.validation).toBe('Enter a valid offset');
    });

    it('edits membership in the table and confirms removal of Easy Edit settings', async () => {
        const { document, view } = setup('sample-select');
        expect(view.queryByRole('combobox', { name: 'Assignment' })).toBeNull();
        expect(view.queryByText('Available')).toBeNull();
        await fireEvent.click(view.getByRole('checkbox', { name: 'Show only assigned' }));
        await fireEvent.click(view.getByRole('button', { name: 'Receive channel assign: Available' }));
        await fireEvent.click(view.getByRole('option', { name: '=Sample' }));
        expect(document.programAssignmentId).toBe(2);
        expect(document.canSave).toBe(true);
        await fireEvent.click(view.getByRole('button', { name: 'Receive channel assign: Duplicate (1)' }));
        await fireEvent.click(view.getByRole('option', { name: 'Off' }));
        expect(view.getByRole('dialog', { name: 'Remove assignment?' })).toBeTruthy();
        await fireEvent.click(view.getByRole('button', { name: 'Remove' }));
        expect(view.queryByRole('button', { name: 'Receive channel assign: Duplicate (1)' })).toBeNull();
        await act(() => document.draft.undo());
        expect(document.draft.values['assignments.0.level_offset']).toBe(11);
    });

    it('uses envelope handles to edit only Program rate offsets with atomic undo', async () => {
        const { document, view } = setup('easy-edit');
        await fireEvent.click(view.getByRole('button', { name: 'Amp EG' }));
        const attack = view.getByRole('button', { name: /^Peak:.*offset 0, base 64, effective 64/ });
        await fireEvent.keyDown(attack, { key: 'ArrowLeft' });
        await fireEvent.keyUp(attack, { key: 'ArrowLeft' });
        expect(document.draft.changes).toEqual({ 'assignments.0.amp_attack_offset': 1 });
        expect(view.container.querySelector('[data-trace="base-envelope"]')).toBeTruthy();
        await act(() => document.draft.undo());
        expect(document.draft.dirty).toBe(false);
    });

    it('edits inclusive key/velocity limits in the shared mapping without changing key shift', async () => {
        const { document, view } = setup('easy-edit');
        await fireEvent.click(view.getByRole('button', { name: 'Key/Velocity' }));
        expect(view.getByRole('group', { name: /^Keyboard/ })).toBeTruthy();
        await fireEvent.keyDown(view.getByRole('button', { name: 'Low key limit' }), {
            key: 'ArrowRight',
        });
        expect(document.draft.values['assignments.0.key_low']).toBe(1);
        expect(view.queryByRole('button', { name: 'Keyboard view: Mapping' })).toBeNull();
        expect(view.getByRole('button', { name: 'Mapping Editor' })).toHaveProperty('disabled', true);
        await fireEvent.input(view.getByRole('spinbutton', { name: 'High velocity' }), { target: { value: '126' } });
        expect(document.draft.values['assignments.0.velocity_high']).toBe(126);
        expect(document.draft.values['assignments.0.key_shift']).toBe(0);
        await act(() => document.draft.undo());
        expect(document.draft.values['assignments.0.velocity_high']).toBe(127);
    });
    it('opens Setup on Mix & Portamento without duplicating primary workspace controls', async () => {
        const { document, view } = setup();
        expect(view.getByRole('spinbutton', { name: 'Level' })).toHaveProperty('value', '100');
        expect(view.getByRole('spinbutton', { name: 'Transpose' })).toHaveProperty('value', '0');
        expect(view.getByRole('button', { name: 'Type: Rate / Fingered' }).getAttribute('aria-pressed')).toBe('true');
        expect(view.queryByRole('button', { name: 'Setup' })).toBeNull();
        expect(view.queryByRole('button', { name: 'Save' })).toBeNull();
        await fireEvent.input(view.getByRole('spinbutton', { name: 'Level' }), { target: { value: '73' } });
        expect(document.draft.changes).toEqual({ level: 73 });
        await act(() => document.draft.undo());
        expect(view.getByRole('spinbutton', { name: 'Level' })).toHaveProperty('value', '100');
        expect(document.draft.dirty).toBe(false);
    });

    it('resets all sixteen words through A-to-B-to-A type selection and undoes each selection atomically', async () => {
        const { document, format, view } = setup('effects');
        const original = { ...document.draft.values };
        expect(view.getByRole('button', { name: 'Select Ef1' })).toBeTruthy();
        expect(view.getByRole('button', { name: 'Select Ef6' })).toBeTruthy();
        await fireEvent.click(view.getByRole('button', { name: 'Select Ef1' }));
        const chooser = view.getByRole('combobox', { name: 'Effect type' });
        await fireEvent.input(chooser, { target: { value: 'AutoSyn' } });
        await fireEvent.click(view.getByRole('option', { name: '2: AutoSyn' }));
        expect(document.draft.values['effects.1.type']).toBe(2);
        for (let word = 0; word < 16; ++word)
            expect(document.draft.values[`effects.1.words.${word}`]).toBe(format.effects[1]!.resetWords[word]);
        const selectedB = { ...document.draft.values };
        await fireEvent.input(chooser, { target: { value: 'Scratch' } });
        await fireEvent.click(view.getByRole('option', { name: '1: Scratch' }));
        expect(document.draft.values['effects.1.type']).toBe(1);
        expect(document.draft.values['effects.1.reset']).toBe(true);
        expect(document.draft.dirty).toBe(true);
        for (let word = 0; word < 16; ++word)
            expect(document.draft.values[`effects.1.words.${word}`]).toBe(format.effects[0]!.resetWords[word]);
        await act(() => document.draft.undo());
        expect(document.draft.values).toEqual(selectedB);
        await act(() => document.draft.undo());
        expect(document.draft.values).toEqual(original);
        expect(document.draft.canUndo).toBe(false);
        expect(document.draft.dirty).toBe(false);
    });

    it('shows routing and algorithm parameters together without a secondary tab row', async () => {
        const { document, view } = setup('effects');
        expect(view.queryByRole('button', { name: 'Parameters' })).toBeNull();
        expect(view.queryByRole('button', { name: 'Routing' })).toBeNull();
        expect(view.getByRole('combobox', { name: 'Effect type' })).toHaveProperty('value', '1: Scratch');
        const parameter = view.getByRole('spinbutton', { name: 'Parameter 1' });
        await fireEvent.input(parameter, { target: { value: '99' } });
        expect(document.draft.changes).toEqual({ 'effects.1.words.0': 99 });
        expect(view.queryByRole('spinbutton', { name: 'Parameter 16' })).toBeNull();
        expect(document.draft.values['effects.1.words.15']).toBe(215);
        expect(view.getByRole('button', { name: 'Select Ef1' })).toBeTruthy();
    });

    it('selects duplicate stored targets by row ordinal and never merges their changes', async () => {
        const { document, view } = setup('easy-edit');
        const assignment = view.getByRole('button', { name: 'Sample/Bank' });
        await fireEvent.click(assignment);
        expect(view.getByRole('option', { name: 'Duplicate (Sample) · 1' })).toBeTruthy();
        await fireEvent.click(view.getByRole('option', { name: 'Duplicate (Sample) · 2' }));
        const level = view.getByRole('spinbutton', { name: 'Level offset' });
        expect(level).toHaveProperty('value', '22');
        await fireEvent.input(level, { target: { value: '55' } });
        expect(document.draft.changes).toEqual({ 'assignments.1.level_offset': 55 });
        expect(document.draft.values['assignments.0.level_offset']).toBe(11);
    });

    it('keeps later-only effect slots and StepWave outside the native editor', async () => {
        const { navigation, view } = setup('effects', true);
        expect(view.getByRole('button', { name: 'Select Ef3' })).toBeTruthy();
        expect(view.queryByRole('button', { name: 'Select Ef4' })).toBeNull();
        await act(() => navigation.selectTab('control'));
        expect(view.getByRole('button', { name: 'Controllers' })).toBeTruthy();
        expect(view.queryByRole('button', { name: 'StepWave' })).toBeNull();
    });

    it.each(['read-only', 'inactive'] as const)(
        'disables numeric and effect selection controls while %s',
        async (state) => {
            const { document, navigation, view } = setup('setup', false, state !== 'read-only', state === 'inactive');
            expect(view.getByRole('spinbutton', { name: 'Level' })).toHaveProperty('disabled', true);
            await act(() => navigation.selectTab('effects'));
            expect(view.getByRole('combobox', { name: 'Effect type' })).toHaveProperty('disabled', true);
            expect(document.draft.dirty).toBe(false);
        },
    );
});
