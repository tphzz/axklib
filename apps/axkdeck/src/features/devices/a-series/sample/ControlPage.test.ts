import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import { fireEvent, render, within } from '@testing-library/svelte';
import { expect, it } from 'vitest';
import ControlPage from './ControlPage.svelte';
import ParameterField from './ParameterField.svelte';
import { sampleTabs } from './fields';
import { EditorDraft } from '../../../object-editor/draft.svelte';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';

it('edits six controller rows independently through searchable controls', async () => {
    const pages = sampleTabs.find((tab) => tab.id === 'midi-ctrl')!.pages;
    expect(pages.map((page) => page.id)).toEqual(['midi', 'control']);
    const page = pages[1]!;
    const draft = new EditorDraft(Object.fromEntries(page.fields.map((field) => [field.key, 0])));
    const document = {
        draft,
        detail: {
            editing: {
                blockedParameters: [],
                blockedParameterReasons: {},
                ...sampleFormatFixture(),
                unavailableParameters: {},
            },
        },
        inputErrors: {},
    } as unknown as ObjectEditorDocument;
    const view = render(ControlPage, { document, page, disabled: false });
    expect(view.getAllByRole('row')).toHaveLength(7);
    expect(view.getAllByRole('combobox')).toHaveLength(18);
    for (const label of ['Controller', 'Function']) {
        const header = view.getByRole('columnheader', { name: new RegExp(`^${label}`) });
        expect(within(header).getByLabelText('A4000/A5000 extension').title).toBe(
            'Additional settings are available in a4k/a5k format',
        );
    }
    for (const label of ['Type', 'Range'])
        expect(view.getByRole('columnheader', { name: label }).querySelector('.extended-parameter')).toBeNull();
    await fireEvent.input(view.getByRole('spinbutton', { name: 'Control 6 Range' }), { target: { value: '25' } });
    expect(draft.changes).toEqual({ 'controls.6.range': 25 });
    const editing = document.detail!.editing!;
    const parameterCapabilities = { ...editing.parameterCapabilities };
    for (const [key, capability] of Object.entries(parameterCapabilities))
        parameterCapabilities[key] = { ...capability, a4000A5000: capability.a3000 };
    await view.rerender({
        document: {
            ...document,
            detail: { ...document.detail!, editing: { ...editing, parameterCapabilities } },
        } as ObjectEditorDocument,
    });
    expect(view.container.querySelector('thead .extended-parameter')).toBeNull();
});

it('renders missing parameters without misleading editable or truncated inputs', () => {
    const field = sampleTabs
        .find((tab) => tab.id === 'map-out')!
        .pages[0]!.fields.find((field) => field.key === 'output1_level')!;
    const view = render(ParameterField, { field, draft: new EditorDraft({}) });
    expect(view.getByText('Unavailable')).toBeTruthy();
    expect(view.queryByRole('spinbutton')).toBeNull();
    expect(view.queryByRole('slider')).toBeNull();
});
