import type { ObjectDetail } from '../../../../lib/transport';
import type { ProgramEditingSnapshot, ProgramEditorFormat, ProgramParameterEdit } from '../../../../lib/objectEditing';
import type { EditorValue, EditorValues } from '../../../object-editor/draft.svelte';
import type { ProgramDraft } from './draft.svelte';

function put(target: Record<string, unknown>, path: string, value: unknown): void {
    const keys = path.split('.');
    for (const key of keys.slice(0, -1)) {
        target[key] ??= {};
        target = target[key] as Record<string, unknown>;
    }
    target[keys.at(-1)!] = value;
}
function wireValue(path: string, value: EditorValue): unknown {
    const field = path.split('.').at(-1);
    if (field === 'receive')
        return value === -1
            ? 'inherit'
            : value === 16
              ? 'basic'
              : {
                    port: Number(value) < 16 ? 'a' : 'b',
                    channel: Number(value) < 16 ? Number(value) + 1 : Number(value) - 16,
                };
    if (['portamento', 'mono', 'key_crossfade', 'velocity_crossfade'].includes(field!))
        return value === -1 ? 'inherit' : value === 0 ? 'off' : 'on';
    if (path === 'step_wave.slope') return ['none', 'rising', 'falling', 'both'][Number(value)];
    return value;
}
export function programEdit(
    detail: ObjectDetail,
    changes: EditorValues,
    values: EditorValues,
    format: ProgramEditorFormat,
    draft?: ProgramDraft,
): ProgramParameterEdit {
    const snapshot = detail.editing;
    if (snapshot?.profile !== 'a-series/program' || !snapshot.programNumber)
        throw new Error('Program placement is unavailable');
    const parameters: Record<string, unknown> = {};
    const rows = new Map<number, Record<string, unknown>>();
    const resetSlots = new Set<number>();
    for (const [key, value] of Object.entries(changes)) {
        if (key.startsWith('$assignment.')) continue;
        const row = /^assignments\.(\d+)\.(.+)$/.exec(key);
        if (row) {
            const ordinal = Number(row[1]);
            const patch = rows.get(ordinal) ?? {};
            put(patch, row[2]!, wireValue(row[2]!, value));
            rows.set(ordinal, patch);
            continue;
        }
        const effect = /^effects\.(\d+)\.(.+)$/.exec(key);
        if (effect && (effect[2] === 'type' || effect[2] === 'reset')) resetSlots.add(Number(effect[1]));
        if (effect?.[2] === 'reset') continue;
        const word = /^effects\.(\d+)\.words\.(\d+)$/.exec(key);
        if (word) {
            const type = format.effects.find((item) => item.id === values[`effects.${word[1]}.type`]);
            if (type?.parameters[Number(word[2])]?.editable)
                put(parameters, `effects.${word[1]}.parameters.${Number(word[2]) + 1}`, value);
        } else put(parameters, key, wireValue(key, value));
    }
    // A type reset is an operation, not just a leaf diff. Send every editable final word,
    // including overrides which happen to equal the pre-edit type's stored value.
    for (const slot of resetSlots) {
        const type = format.effects.find((item) => item.id === values[`effects.${slot}.type`]);
        if (!type) throw new Error('Choose a supported effect type');
        put(parameters, `effects.${slot}.type`, type.id);
        put(parameters, `effects.${slot}.reset_parameters`, true);
        for (const word of type.parameters.filter((item) => item.editable))
            put(
                parameters,
                `effects.${slot}.parameters.${word.index + 1}`,
                values[`effects.${slot}.words.${word.index}`],
            );
    }
    const identity = {
        id: 'program-edit',
        partition_index: snapshot.partitionIndex,
        volume_name: snapshot.volumeName,
        program_number: snapshot.programNumber,
        model: snapshot.model,
        expected_payload_sha256: snapshot.payloadSha256,
        parameters,
    };
    if (draft?.membershipChanged)
        return {
            expectedRevision: detail.image.revision,
            operation: {
                ...identity,
                type: 'replace_program_assignments',
                assignments: draft.assignments.map((row) => {
                    const retained = row.retainOrdinal === undefined ? {} : { retain_ordinal: row.retainOrdinal };
                    const patch = rows.get(row.id);
                    if (!patch && row.retainOrdinal !== undefined) return retained;
                    if (row.kind === 'UNKNOWN' || !row.name) throw new Error('Assignment identity is unavailable');
                    return {
                        ...retained,
                        [row.kind === 'SBAC' ? 'sample_bank' : 'sample']: row.name,
                        parameters: patch ?? {},
                    };
                }),
            },
        };
    return {
        expectedRevision: detail.image.revision,
        operation: {
            ...identity,
            type: 'update_program_parameters',
            assignments: [...rows].map(([ordinal, parameters]) => {
                const row = snapshot.assignments.find((item) => item.ordinal === ordinal);
                if (!row || row.kind === 'UNKNOWN') throw new Error('Assignment identity is unavailable');
                return { ordinal, expected_target_kind: row.kind, expected_target_name: row.name, parameters };
            }),
        },
    };
}

export function validateProgram(
    values: EditorValues,
    changes: EditorValues,
    snapshot: ProgramEditingSnapshot,
    format?: ProgramEditorFormat,
    draft?: ProgramDraft,
): string {
    if (!snapshot.editable) return snapshot.reason;
    if (!format) return 'Loading Program parameter catalog';
    if (draft && draft.assignments.length > 999) return 'A Program can contain at most 999 assignments';
    for (const [key, value] of Object.entries(changes)) {
        if (key.startsWith('$assignment.')) continue;
        if (/^effects\.[1-6]\.reset$/.test(key) && value === true) continue;
        const word = /^effects\.(\d+)\.words\.(\d+)$/.exec(key);
        if (word) {
            const type = format.effects.find((item) => item.id === values[`effects.${word[1]}.type`]);
            const domain = type?.parameters[Number(word[2])];
            if (!domain) return 'Unsupported effect parameter';
            if (!domain.editable) {
                if (values[`effects.${word[1]}.reset`] && value === type!.resetWords[Number(word[2])]) continue;
                return 'Hidden effect parameters cannot be edited';
            }
            if (typeof value !== 'number' || !Number.isInteger(value) || value < domain.min || value > domain.max)
                return `${domain.label}: choose a supported value`;
            continue;
        }
        const domain = format.fields.find(
            (field) => field.key === key.replace(/^assignments\.\d+\./, 'assignments.*.'),
        );
        const id = /^assignments\.(\d+)\./.exec(key)?.[1];
        const added =
            id !== undefined &&
            draft?.assignments.some((row) => row.id === Number(id) && row.retainOrdinal === undefined);
        if (!domain || (!(key in snapshot.values) && !(added && domain.defaultValue !== undefined)))
            return 'This Program parameter is unavailable';
        if (
            domain.boolean
                ? typeof value !== 'boolean'
                : typeof value !== 'number' ||
                  !Number.isInteger(value) ||
                  value < domain.min ||
                  value > domain.max ||
                  (domain.allowedValues && !domain.allowedValues.includes(value))
        )
            return 'Choose a supported Program parameter value';
    }
    for (const row of draft?.assignments ?? snapshot.assignments.map((row) => ({ ...row, id: row.ordinal }))) {
        const prefix = `assignments.${row.id}.`;
        if (row.kind === 'UNKNOWN' && Object.keys(changes).some((key) => key.startsWith(prefix)))
            return 'Unknown assignment targets are read-only';
        for (const group of ['key', 'velocity']) {
            const low = `${prefix}${group}_low`,
                high = `${prefix}${group}_high`;
            if ((low in changes || high in changes) && Number(values[low]) > Number(values[high]))
                return `${group === 'key' ? 'Key' : 'Velocity'} limits must be ordered`;
        }
    }
    return '';
}
