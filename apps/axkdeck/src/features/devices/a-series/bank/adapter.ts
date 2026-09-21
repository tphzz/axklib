import type { BankParameterEdit, SampleEditingSnapshot } from '../../../../lib/objectEditing';
import type { ObjectDetail } from '../../../../lib/transport';
import type { EditorValues } from '../../../object-editor/draft.svelte';
import { sampleValues, sampleEdit, validateSample } from '../sample/adapter';
import { bankValues, overrideKey } from './draft.svelte';

export function bankEdit(detail: ObjectDetail, changes: EditorValues, values: EditorValues): BankParameterEdit {
    const snapshot = detail.editing!;
    const units = snapshot.bankOverrides!.units;
    const enabled = units.filter((unit) => values[overrideKey(unit.id)] === true);
    const parameters = sampleEdit(
        detail,
        Object.fromEntries(Object.entries(changes).filter(([key]) => enabled.some((unit) => unit.keys.includes(key)))),
        values,
    ).operation.parameters;
    return {
        expectedRevision: detail.image.revision,
        operation: {
            id: 'bank-edit',
            type: 'update_sample_bank_overrides',
            partition_index: snapshot.partitionIndex,
            volume_name: snapshot.volumeName,
            sample_bank_name: detail.object.name,
            expected_payload_sha256: snapshot.payloadSha256,
            parameters,
            enable: enabled
                .filter((unit) => overrideKey(unit.id) in changes || unit.keys.some((key) => key in changes))
                .map((unit) => unit.id),
            disable: units
                .filter((unit) => values[overrideKey(unit.id)] === false && overrideKey(unit.id) in changes)
                .map((unit) => unit.id),
        },
    };
}

export const aSeriesBankAdapter = {
    profile: 'a-series/sample-bank',
    label: 'A-series',
    values: (snapshot: SampleEditingSnapshot) => bankValues(sampleValues(snapshot), snapshot.bankOverrides!.units),
    edit: bankEdit,
    validate: (values: EditorValues, changes: EditorValues, snapshot: SampleEditingSnapshot): string => {
        if (!snapshot.editable) return snapshot.reason;
        const check: EditorValues = {};
        for (const unit of snapshot.bankOverrides!.units) {
            if (!values[overrideKey(unit.id)]) continue;
            if (overrideKey(unit.id) in changes || unit.keys.some((key) => key in changes))
                for (const key of unit.keys) {
                    if (values[key] === undefined) return 'Choose valid values for every field in the override group';
                    check[key] = values[key]!;
                }
        }
        return validateSample(values, check, snapshot);
    },
};
