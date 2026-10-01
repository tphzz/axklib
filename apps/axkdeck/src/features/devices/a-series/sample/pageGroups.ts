import type { SamplePage } from './fields';

const groups: Record<string, { title: string; keys: string[] }[]> = {
    'mix-key': [
        { title: 'Mix', keys: ['level', 'pan', 'velocity_sensitivity', 'mono_mode'] },
        { title: 'Key mapping', keys: ['root_key', 'key_low', 'key_high', 'key_crossfade'] },
        {
            title: 'Output routing',
            keys: ['output1_destination', 'output1_level', 'output2_destination', 'output2_level'],
        },
    ],
    pitch: [
        { title: 'Tuning', keys: ['coarse_tune', 'fine_tune_cents', 'fixed_pitch', 'random_pitch'] },
        { title: 'Sample portamento', keys: ['portamento_type', 'portamento_rate', 'portamento_time'] },
    ],
    velocity: [
        { title: 'Expansion', keys: ['expand_detune', 'expand_dephase', 'expand_width'] },
        {
            title: 'Velocity range',
            keys: ['velocity_low', 'velocity_high', 'velocity_crossfade', 'velocity_xfade_low', 'velocity_xfade_high'],
        },
    ],
    midi: [
        { title: 'MIDI Set', keys: ['midi_receive_channel', 'alternate_group'] },
        { title: 'Pitch bend', keys: ['pitch_bend_type', 'pitch_bend_range'] },
        { title: 'Velocity', keys: ['velocity_low_limit', 'velocity_offset'] },
    ],
    filter: [
        {
            title: 'Filter',
            keys: ['filter_type', 'filter_cutoff', 'filter_q_width', 'filter_gain', 'filter_cutoff_distance'],
        },
    ],
    'sample-eq': [
        {
            title: 'Sample EQ',
            keys: ['sample_eq_type', 'sample_eq_frequency', 'sample_eq_gain_db', 'sample_eq_width_tenths'],
        },
    ],
    lfo: [
        { title: 'Oscillator', keys: ['lfo.wave', 'lfo.speed', 'lfo.delay_time', 'lfo.key_on_sync'] },
        {
            title: 'Depth',
            keys: [
                'lfo.pitch_mod_depth',
                'lfo.pitch_mod_phase_invert',
                'lfo.cutoff_mod_depth',
                'lfo.cutoff_mod_phase_invert',
                'lfo.amp_mod_depth',
            ],
        },
    ],
};

export function pageGroups(page: SamplePage) {
    const definitions = groups[page.id];
    if (definitions)
        return definitions.map(({ title, keys }) => ({
            title,
            fields: keys.map((key) => page.fields.find((field) => field.key === key)!),
        }));
    if (['aeg', 'feg', 'peg'].includes(page.id))
        return [
            {
                title: 'Rates',
                fields: page.fields.filter((field) => field.key.endsWith('_rate') || field.key.endsWith('attack_mode')),
            },
            { title: 'Levels', fields: page.fields.filter((field) => field.key.endsWith('_level')) },
            {
                title: 'Sensitivity',
                fields: page.fields.filter(
                    (field) =>
                        !field.key.endsWith('_rate') &&
                        !field.key.endsWith('_level') &&
                        !field.key.endsWith('attack_mode'),
                ),
            },
        ];
    return [{ title: page.label, fields: page.fields }];
}
