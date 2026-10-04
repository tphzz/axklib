import type { ProgramEditingSnapshot, ProgramEditorCatalog, ProgramEditorFormat } from '../lib/objectEditing';
import type { ObjectDetail } from '../lib/transport';
import { ObjectEditorDocument } from '../features/object-editor/workflow.svelte';

export function programEditorFixture(native = false, editable = true) {
    const model = native ? 'A3000' : 'A5000';
    const fields: ProgramEditorFormat['fields'] = [
        { key: 'level', min: 0, max: 127 },
        { key: 'transpose', min: -127, max: 127 },
        { key: 'portamento.type', min: 0, max: 3 },
        { key: 'portamento.rate', min: 1, max: 127 },
        { key: 'portamento.time', min: 1, max: 127 },
        { key: 'assignments.*.level_offset', min: -127, max: 127 },
        { key: 'assignments.*.pan_offset', min: -127, max: 127 },
        { key: 'effect_connections.1', min: 0, max: 4 },
    ];
    const values: ProgramEditingSnapshot['values'] = {
        level: 100,
        transpose: 0,
        'portamento.type': 0,
        'portamento.rate': 90,
        'portamento.time': 64,
        'effect_connections.1': 0,
        'assignments.0.level_offset': 11,
        'assignments.0.pan_offset': 0,
        'assignments.1.level_offset': 22,
        'assignments.1.pan_offset': 0,
    };
    for (let slot = 1; slot <= (native ? 3 : 6); ++slot) {
        const prefix = `effects.${slot}`;
        fields.push(
            { key: `${prefix}.type`, min: 1, max: 2 },
            { key: `${prefix}.enabled`, min: 0, max: 1, boolean: true },
            { key: `${prefix}.input_level`, min: 0, max: 127 },
            { key: `${prefix}.output_level`, min: 0, max: 127 },
            { key: `${prefix}.pan`, min: -63, max: 63 },
            { key: `${prefix}.width`, min: -126, max: 0 },
            { key: `${prefix}.destination`, min: 0, max: !native && slot <= 3 ? 8 : 5 },
        );
        Object.assign(values, {
            [`${prefix}.type`]: 1,
            [`${prefix}.enabled`]: true,
            [`${prefix}.input_level`]: 100,
            [`${prefix}.output_level`]: 100,
            [`${prefix}.pan`]: 0,
            [`${prefix}.width`]: 0,
            [`${prefix}.destination`]: 0,
            ...Object.fromEntries(Array.from({ length: 16 }, (_, word) => [`${prefix}.words.${word}`, 200 + word])),
        });
    }
    for (let index = 1; index <= 4; ++index) {
        for (const [name, min, max] of [
            ['device', 0, native ? 125 : 126],
            ['function', 0, native ? 63 : 128],
            ['type', 0, 3],
            ['range', -63, 63],
        ] as const) {
            const key = `controllers.${index}.${name}`;
            fields.push({ key, min, max });
            values[key] = 0;
        }
    }
    if (!native) {
        fields.push(
            { key: 'effect_connections.2', min: 0, max: 4 },
            { key: 'step_wave.step_count', min: 2, max: 16, allowedValues: [2, 3, 4, 6, 8, 12, 16] },
            { key: 'step_wave.slope', min: 0, max: 3 },
        );
        Object.assign(values, { 'effect_connections.2': 0, 'step_wave.step_count': 8, 'step_wave.slope': 0 });
        for (let step = 1; step <= 16; ++step) {
            fields.push({ key: `step_wave.values.${step}`, min: 0, max: 127 });
            values[`step_wave.values.${step}`] = 64;
        }
    }
    const format: ProgramEditorFormat = {
        model,
        fields,
        effects: [1, 2].map((id) => ({
            id,
            label: id === 1 ? 'Scratch' : 'AutoSyn',
            printedNumber: id,
            resetWords: Array.from({ length: 16 }, (_, word) => (id === 1 ? 10 : 40) + word),
            parameters: Array.from({ length: 16 }, (_, index) => ({
                index,
                label: `Parameter ${index + 1}`,
                min: 0,
                max: 1000,
                editable: index < 2,
            })),
        })),
    };
    const catalog: ProgramEditorCatalog = { schemaVersion: 1, formats: [format] };
    for (const name of [
        'receive',
        'amp_attack_offset',
        'amp_decay_offset',
        'amp_release_offset',
        'key_shift',
        'key_low',
        'key_high',
        'velocity_low',
        'velocity_high',
    ]) {
        const defaultValue = name.endsWith('_high') ? 127 : name === 'receive' ? -1 : 0;
        fields.push({
            key: `assignments.*.${name}`,
            min: name === 'receive' ? -1 : name.endsWith('offset') || name === 'key_shift' ? -127 : 0,
            max: name === 'receive' ? (native ? 16 : 32) : 127,
            defaultValue,
        });
        for (const id of [0, 1]) values[`assignments.${id}.${name}`] = defaultValue;
    }
    for (const field of fields)
        if (field.key.startsWith('assignments.*.') && field.defaultValue === undefined) field.defaultValue = 0;
    const editing: ProgramEditingSnapshot = {
        profile: 'a-series/program',
        editable,
        reason: editable ? '' : 'This image is read-only',
        payloadSha256: 'a'.repeat(64),
        partitionIndex: 0,
        volumeName: 'Volume',
        programNumber: 33,
        programName: 'Test',
        storageRevision: native ? 2 : 4,
        model,
        values,
        assignments: [
            { ordinal: 0, kind: 'SBNK', name: 'Duplicate', targetObjectId: 'sample' },
            { ordinal: 1, kind: 'SBNK', name: 'Duplicate', targetObjectId: 'sample' },
        ],
        targets: ['Duplicate', 'Available'].map((name, index) => ({
            objectId: index ? 'available' : 'sample',
            kind: 'SBNK',
            name,
            assignable: true,
            reason: '',
            available: true,
            values: {
                root_key: 60,
                key_low: 36,
                key_high: 84,
                velocity_low: 1,
                velocity_high: 127,
                'aeg.attack_rate': 64,
                'aeg.decay_rate': 80,
                'aeg.release_rate': 100,
                'aeg.sustain_level': 90,
                'aeg.attack_mode': 0,
            },
            overrideKeys: [],
            members: [],
        })),
    };
    const detail = {
        image: { revision: 1 },
        object: { id: 'program', key: 'program', name: '033', type: 'PROG' },
        editing,
    } as unknown as ObjectDetail;
    return { document: new ObjectEditorDocument(1, detail, {}, catalog), catalog, format, editing };
}
