import type { SampleEditingSnapshot, SampleStorageFormat } from '../lib/objectEditing';
import { sampleFields } from '../features/devices/a-series/sample/fields';

export function sampleFormatFixture(
    format: SampleStorageFormat = 'A4000_A5000_224',
): Pick<SampleEditingSnapshot, 'sampleFormat' | 'parameterCapabilities' | 'formatConversions' | 'canConvertFormat'> {
    const native = format === 'A3000_188';
    const capabilities: SampleEditingSnapshot['parameterCapabilities'] = {};
    for (const field of sampleFields) {
        if (field.key.startsWith('playback.')) continue;
        const domain = {
            minimum: field.min,
            maximum: field.max,
            extraValue: field.special?.[0]?.value ?? null,
            offset: 0,
            mask: 255,
        };
        let older = { ...domain } as typeof domain | null;
        let later = { ...domain } as typeof domain | null;
        if (
            [
                'sample_eq_type',
                'velocity_xfade_low',
                'velocity_xfade_high',
                'portamento_rate',
                'portamento_time',
            ].includes(field.key)
        )
            older = null;
        if (field.key === 'velocity_crossfade') later = null;
        if (field.key === 'portamento_type') older!.maximum = 1;
        if (field.key === 'coarse_tune') {
            older!.minimum = -127;
            older!.maximum = 127;
        }
        if (field.key === 'pitch_bend_type') older!.maximum = 13;
        if (field.key === 'aeg.attack_mode') older!.maximum = 1;
        if (field.key.startsWith('controls.') && field.key.endsWith('.device')) older!.maximum = 125;
        if (field.key.startsWith('controls.') && field.key.endsWith('.function')) older!.maximum = 21;
        if (field.key === 'output1_destination') older!.maximum = 4;
        if (field.key === 'output2_destination') older!.maximum = 5;
        const active = native ? older : later;
        capabilities[field.key] = {
            available: !!active,
            editable: !!active,
            valid: !!active,
            reason: active ? '' : 'Convert explicitly to the other Sample format to edit this setting.',
            a3000: older,
            a4000A5000: later,
            storedValue: null,
            a5000Minimum: field.key.startsWith('output') && field.key.endsWith('_destination') ? 10 : null,
        };
    }
    return {
        sampleFormat: {
            format,
            structurallyValid: format !== 'UNKNOWN',
            parameterBytes: native ? 188 : 224,
            headerRevision: native ? 2 : 4,
            olderBodyBytes: 308,
            laterBodyBytes: native ? 0 : 344,
            diagnostics: [],
            parameterIssues: [],
            requiresA5000: false,
            extensionDiffersFromPrefixDefaults: native ? null : false,
        },
        parameterCapabilities: capabilities,
        canConvertFormat: true,
        formatConversions: [
            {
                targetFormat: native ? 'A4000_A5000_224' : 'A3000_188',
                allowed: true,
                changes: ['Keep the Sample identity and Wave Data unchanged.'],
                blockers: [],
            },
        ],
    };
}
