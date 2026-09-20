import type { ObjectDetail } from '../../../../lib/transport';
import type { ObjectParameterEdit, SampleEditingSnapshot } from '../../../../lib/objectEditing';
import type { EditorValues } from '../../../object-editor/draft.svelte';
import { sampleFields } from './fields';
import { formatField, parameterAllowed } from './formatCapabilities';

export function sampleValues(snapshot: SampleEditingSnapshot): EditorValues {
    const result: EditorValues = {};
    const visit = (value: object, prefix = ''): void => {
        for (const [key, child] of Object.entries(value)) {
            const path = prefix ? `${prefix}.${key}` : key;
            if (typeof child === 'number' || typeof child === 'boolean') result[path] = child;
            else if (child && typeof child === 'object') visit(child, path);
        }
    };
    visit(snapshot.parameters);
    for (const [key, capability] of Object.entries(snapshot.parameterCapabilities))
        if (capability.available && capability.storedValue !== null && !(key in result))
            result[key] = capability.storedValue;
    visit(snapshot.playbackWindow, 'playback');
    return result;
}

export function sampleEdit(detail: ObjectDetail, changes: EditorValues, values: EditorValues): ObjectParameterEdit {
    const snapshot = detail.editing!;
    const parameters: Record<string, unknown> = {};
    for (const [path, value] of Object.entries(changes)) {
        if (path.startsWith('playback.')) continue;
        const keys = path.split('.');
        let group = parameters;
        for (const key of keys.slice(0, -1)) {
            group[key] ??= {};
            group = group[key] as Record<string, unknown>;
        }
        group[keys.at(-1)!] = value;
    }
    return {
        expectedRevision: detail.image.revision,
        operation: {
            id: 'sample-edit',
            type: 'update_sbnk_parameters',
            partition_index: snapshot.partitionIndex,
            volume_name: snapshot.volumeName,
            sample_name: detail.object.name,
            expected_payload_sha256: snapshot.payloadSha256,
            parameters,
            ...(Object.keys(changes).some((key) => key.startsWith('playback.'))
                ? {
                      playback_window: {
                          start_frame: Number(values['playback.start_frame']),
                          length_frames: Number(values['playback.length_frames']),
                      },
                  }
                : {}),
        },
    };
}

export function validateSample(values: EditorValues, changes: EditorValues, snapshot: SampleEditingSnapshot): string {
    if (!snapshot.editable) return snapshot.reason;
    for (const key of Object.keys(changes)) {
        const definition = sampleFields.find((candidate) => candidate.key === key);
        const field = definition ? formatField(definition, snapshot) : undefined;
        if (!field) return 'This parameter is not editable';
        if (!(key in sampleValues(snapshot))) return `${field.label} is unavailable in this Sample`;
        const value = changes[key]!;
        if (!key.startsWith('playback.') && !parameterAllowed(snapshot, key, value))
            return `${field.label}: this value is not supported by the stored Sample format`;
        if (
            field.boolean
                ? typeof value !== 'boolean'
                : typeof value !== 'number' ||
                  (field.options
                      ? !field.options.some((option) => option.value === value)
                      : !field.special?.some((option) => option.value === value) &&
                        (value < field.min || value > field.max))
        )
            return `${field.label}: choose a supported value`;
        if (snapshot.blockedParameters.includes(key))
            return snapshot.blockedParameterReasons[key] || `${field.label} is read-only for this Sample`;
        if (key.startsWith('playback.') && !snapshot.canEditPlayback)
            return 'Playback bounds are preserved for this Sample';
        if (typeof changes[key] === 'number' && !Number.isInteger(changes[key]))
            return 'Parameter values must be whole numbers';
    }
    const touched = (...keys: string[]): boolean => keys.some((key) => key in changes);
    const n = (key: string): number => Number(values[key]);
    if (touched('key_low', 'key_high', 'root_key')) {
        const low = n('key_low') === 255 ? n('root_key') : n('key_low');
        const high = n('key_high') === 128 ? n('root_key') : n('key_high');
        if (low > high) return 'Low key must not exceed high key';
    }
    for (const [low, high, label] of [
        ['velocity_low', 'velocity_high', 'Velocity'],
        ['level_scaling_break1', 'level_scaling_break2', 'Level scaling'],
        ['filter_scaling_break1', 'filter_scaling_break2', 'Filter scaling'],
    ])
        if (touched(low!, high!) && n(low!) > n(high!))
            return `${label}: the lower bound must not exceed the upper bound`;
    if (
        touched('playback.start_frame', 'playback.length_frames', 'loop_start_frame', 'loop_length_frames', 'loop_mode')
    ) {
        const start = n('playback.start_frame');
        const end = start + n('playback.length_frames');
        const loopStart = n('loop_start_frame');
        const loopEnd = loopStart + n('loop_length_frames');
        if (start < 0 || n('playback.length_frames') <= 0 || end > snapshot.maximumFrames)
            return 'Playback must fit inside the stored Wave Data';
        const empty = n('loop_length_frames') === 0;
        if (
            empty
                ? loopStart !== 0 || [1, 2].includes(n('loop_mode'))
                : n('loop_length_frames') < 0 || loopStart < start || loopEnd > end
        )
            return 'Loop bounds must fit inside playback; repeating modes require a non-empty loop';
    }
    return '';
}

export const aSeriesSampleAdapter = {
    profile: 'a-series/sample',
    label: 'A-series',
    values: sampleValues,
    edit: sampleEdit,
    validate: validateSample,
};
