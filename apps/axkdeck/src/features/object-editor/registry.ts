import { aSeriesSampleAdapter } from '../devices/a-series/sample/adapter';
import { aSeriesBankAdapter } from '../devices/a-series/bank/adapter';
import type { ObjectDetail } from '../../lib/transport';
import type { EditingSnapshot, ObjectParameterEdit, ProgramEditorFormat } from '../../lib/objectEditing';
import type { EditorValues } from './draft.svelte';
import { programEdit, validateProgram } from '../devices/a-series/program/adapter';
import { ProgramDraft } from '../devices/a-series/program/draft.svelte';
import type { EditorDraftState } from './draft.svelte';

// Editor profiles describe device/object semantics, independently of server backends.
export function objectEditorAdapter(detail: ObjectDetail, draft?: EditorDraftState) {
    if (detail.editing?.profile === 'a-series/program')
        return {
            profile: 'a-series/program',
            values: (snapshot: EditingSnapshot) =>
                snapshot.profile === 'a-series/program' ? { ...snapshot.values } : {},
            edit: (
                detail: ObjectDetail,
                changes: EditorValues,
                values: EditorValues,
                format?: ProgramEditorFormat,
            ): ObjectParameterEdit => {
                if (!format) throw new Error('Program parameter catalog is unavailable');
                return programEdit(detail, changes, values, format, draft instanceof ProgramDraft ? draft : undefined);
            },
            validate: (
                values: EditorValues,
                changes: EditorValues,
                snapshot: EditingSnapshot,
                format?: ProgramEditorFormat,
            ) =>
                snapshot.profile === 'a-series/program'
                    ? validateProgram(
                          values,
                          changes,
                          snapshot,
                          format,
                          draft instanceof ProgramDraft ? draft : undefined,
                      )
                    : 'Wrong editor profile',
        };
    const adapter =
        detail.editing?.profile === aSeriesSampleAdapter.profile
            ? aSeriesSampleAdapter
            : detail.editing?.profile === aSeriesBankAdapter.profile && detail.editing.bankOverrides
              ? aSeriesBankAdapter
              : null;
    return adapter
        ? {
              profile: adapter.profile,
              values: (snapshot: EditingSnapshot) =>
                  snapshot.profile === 'a-series/program' ? {} : adapter.values(snapshot),
              edit: (
                  detail: ObjectDetail,
                  changes: EditorValues,
                  values: EditorValues,
                  _format?: ProgramEditorFormat,
              ): ObjectParameterEdit => adapter.edit(detail, changes, values),
              validate: (
                  values: EditorValues,
                  changes: EditorValues,
                  snapshot: EditingSnapshot,
                  _format?: ProgramEditorFormat,
              ) =>
                  snapshot.profile === 'a-series/program'
                      ? 'Wrong editor profile'
                      : adapter.validate(values, changes, snapshot),
          }
        : null;
}

export async function prepareEditorDraft(
    ...args: Parameters<typeof import('../devices/a-series/sample/audition').prepareSampleDraft>
) {
    if (args[3].profile !== aSeriesSampleAdapter.profile)
        throw new Error('Draft preview is unavailable for this profile');
    const { prepareSampleDraft } = await import('../devices/a-series/sample/audition');
    return prepareSampleDraft(...args);
}
