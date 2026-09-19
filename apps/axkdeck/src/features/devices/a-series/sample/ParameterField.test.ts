import { fireEvent, render } from '@testing-library/svelte';
import { expect, it } from 'vitest';
import ParameterField from './ParameterField.svelte';
import { sampleFields } from './fields';
import { EditorDraft } from '../../../object-editor/draft.svelte';

it.each([
    "This Sample's short parameter layout does not store this setting.",
    'The stored value is outside the supported range. It is preserved unchanged.',
])('explains unavailable values on label and value focus: %s', async (reason) => {
    const field = sampleFields.find((item) => item.key === 'output1_destination')!;
    const draft = new EditorDraft({});
    const view = render(ParameterField, { field, draft, unavailableReason: reason });
    await fireEvent.focus(view.getByRole('button', { name: field.label }));
    expect(view.getByRole('tooltip').textContent).toBe(reason);
    await fireEvent.blur(view.getByRole('button', { name: field.label }));
    await fireEvent.focus(view.getByRole('button', { name: `${field.label}: Unavailable` }));
    expect(view.getByRole('tooltip').textContent).toBe(reason);
    expect(view.queryByRole('combobox')).toBeNull();
    expect(draft.dirty).toBe(false);
});
