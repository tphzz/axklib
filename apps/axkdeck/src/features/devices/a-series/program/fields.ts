import type { ProgramEditorFormat } from '../../../../lib/objectEditing';
import type { EditorValues } from '../../../object-editor/draft.svelte';
import { connectionLabels, effectDestinations, programRoutes } from './routing';

export interface ProgramOption {
    value: number;
    label: string;
    extended?: boolean;
    a5000Only?: boolean;
    disabled?: boolean;
    reason?: string;
}
export interface ProgramField {
    key: string;
    label: string;
    min: number;
    max: number;
    boolean?: boolean;
    options?: ProgramOption[];
    extended?: boolean;
    a5000Only?: boolean;
    reason?: string;
}
export const programTabs = [
    { id: 'sample-select', label: 'Sample Select', pages: [] },
    {
        id: 'easy-edit',
        label: 'Easy Edit',
        pages: [
            { id: 'mix', label: 'Mix/Out' },
            { id: 'filter', label: 'Filter' },
            { id: 'pitch', label: 'Pitch' },
            { id: 'amp', label: 'Amp EG' },
            { id: 'range', label: 'Key/Velocity' },
            { id: 'assignment-control', label: 'Control' },
        ],
    },
    {
        id: 'effects',
        label: 'Effects',
        pages: [
            { id: 'routing', label: 'Routing' },
            { id: 'parameters', label: 'Parameters' },
        ],
    },
    {
        id: 'setup',
        label: 'Setup',
        pages: [
            { id: 'mix-portamento', label: 'Mix & Portamento' },
            { id: 'ad', label: 'A/D Input' },
            { id: 'sh', label: 'S/H Speed' },
        ],
    },
    {
        id: 'control',
        label: 'Control',
        pages: [
            { id: 'controllers', label: 'Controllers' },
            { id: 'channels', label: 'Channel Setup' },
            { id: 'program-lfo', label: 'Program LFO' },
            { id: 'stepwave', label: 'StepWave' },
        ],
    },
];
export const assignmentPages: Record<string, string[]> = {
    mix: ['level_offset', 'pan_offset', 'output1', 'output1_level_offset', 'output2', 'output2_level_offset'],
    filter: ['filter_cutoff_offset', 'filter_gain_offset', 'filter_q_offset', 'filter_cutoff_distance_offset'],
    pitch: ['coarse_tune_offset', 'fine_tune_offset'],
    amp: ['amp_attack_offset', 'amp_decay_offset', 'amp_release_offset'],
    range: [
        'key_low',
        'key_high',
        'key_shift',
        'key_crossfade',
        'velocity_low',
        'velocity_high',
        'velocity_crossfade',
        'velocity_sensitivity_offset',
        'low_velocity_crossfade_offset',
        'high_velocity_crossfade_offset',
    ],
    'assignment-control': ['receive', 'portamento', 'mono', 'alternate_group', 'midi_control'],
};
const labels: Record<string, string> = {
    level: 'Level',
    transpose: 'Transpose',
    type: 'Type',
    rate: 'Rate',
    time: 'Time',
    cycle: 'Cycle',
    sync: 'Sync',
    wave: 'Wave',
    initial_phase: 'Initial phase',
    tempo: 'Tempo',
    reset_channel: 'Reset channel',
    reset_note: 'Reset note',
    sample_hold_speed: 'S/H speed',
    step_count: 'Steps',
    slope: 'Slope',
    enabled: 'Enabled',
    source: 'Source',
    pan: 'Pan',
    destination: 'Output',
    input_level: 'Input level',
    output_level: 'Output level',
    width: 'Width',
    device: 'Controller',
    function: 'Function',
    range: 'Range',
    level_offset: 'Level offset',
    pan_offset: 'Pan offset',
    output1: 'Output 1',
    output2: 'Output 2',
    output1_level_offset: 'Output 1 level offset',
    output2_level_offset: 'Output 2 level offset',
    filter_cutoff_offset: 'Cutoff offset',
    filter_gain_offset: 'Gain offset',
    filter_q_offset: 'Q/width offset',
    filter_cutoff_distance_offset: 'Cutoff distance offset',
    coarse_tune_offset: 'Coarse tune offset',
    fine_tune_offset: 'Fine tune offset',
    amp_attack_offset: 'Attack rate offset',
    amp_decay_offset: 'Decay rate offset',
    amp_release_offset: 'Release rate offset',
    key_low: 'Low key',
    key_high: 'High key',
    key_shift: 'Key shift',
    key_crossfade: 'Key crossfade',
    velocity_low: 'Low velocity',
    velocity_high: 'High velocity',
    velocity_crossfade: 'Velocity crossfade',
    velocity_sensitivity_offset: 'Velocity sensitivity offset',
    low_velocity_crossfade_offset: 'Low velocity crossfade offset',
    high_velocity_crossfade_offset: 'High velocity crossfade offset',
    receive: 'Receive channel assign',
    portamento: 'Portamento',
    mono: 'Mono',
    alternate_group: 'Alternate group',
    midi_control: 'MIDI control',
};
const choices = (labels: string[], first = 0): ProgramOption[] =>
    labels.map((label, index) => ({ value: index + first, label }));
export function receiveOptions(native: boolean, reset = false): ProgramOption[] {
    return [
        ...(reset
            ? [
                  { value: -2, label: 'Off' },
                  { value: -1, label: 'Audition' },
              ]
            : [{ value: -1, label: '=Sample' }]),
        ...Array.from({ length: 16 }, (_, i) => ({ value: i, label: `A${String(i + 1).padStart(2, '0')}` })),
        { value: 16, label: 'Basic channel' },
        ...(!native
            ? Array.from({ length: 16 }, (_, i) => ({
                  value: i + 17,
                  label: `B${String(i + 1).padStart(2, '0')}`,
                  a5000Only: true,
              }))
            : []),
    ];
}
export function outputOptions(output: number, native: boolean, inherit: boolean): ProgramOption[] {
    const names =
        output === 1
            ? [
                  'Off',
                  'Stereo Out',
                  'Ef1',
                  'Ef2',
                  'Ef3',
                  'Assign L&R',
                  'Assign 1&2',
                  'Assign 3&4',
                  'Assign 5&6',
                  'DIG & OPT',
                  'Ef4',
                  'Ef5',
                  'Ef6',
              ]
            : [
                  'Off',
                  'Assign L&R',
                  'Assign 1&2',
                  'Assign 3&4',
                  'Assign 5&6',
                  'DIG & OPT',
                  'Stereo Out',
                  'Ef1',
                  'Ef2',
                  'Ef3',
                  'Ef4',
                  'Ef5',
                  'Ef6',
              ];
    return [
        ...(inherit ? [{ value: -1, label: '=Sample' }] : []),
        ...choices(native ? names.slice(0, output === 1 ? 5 : 6) : names).map((item) => ({
            ...item,
            a5000Only: item.value >= 10,
            extended: item.value > (output === 1 ? 4 : 5),
        })),
    ];
}
export function controllerFunctions(format: ProgramEditorFormat, values: EditorValues): ProgramOption[] {
    const options = choices([
        'Off',
        'Portamento rate/time',
        'Sample S/H speed',
        'A/D L&R pan',
        'A/D L&R level',
        'Program level',
    ]);
    const starts = [8, 26, 44, 72, 91, 110];
    for (let slot = 1; slot <= (format.model === 'A3000' ? 3 : 6); slot++) {
        const type = format.effects.find((item) => item.id === values[`effects.${slot}.type`]);
        const start = starts[slot - 1]!;
        const level = slot <= 3 ? start - 2 : start + 16,
            pan = level + 1,
            width = slot <= 3 ? 59 + slot : level + 2;
        options.push(
            ...[
                { value: level, label: `Ef${slot} output level` },
                { value: pan, label: `Ef${slot} pan` },
                { value: width, label: `Ef${slot} width` },
            ].map((item) => ({ ...item, a5000Only: slot > 3 })),
        );
        for (let word = 0; word < 16; word++) {
            const parameter = type?.parameters[word];
            options.push({
                value: start + word,
                label: `Ef${slot} ${word + 1}: ${parameter?.label || 'Unused'}`,
                a5000Only: slot > 3,
                disabled: !parameter?.editable,
                reason: !parameter?.editable ? 'Not controllable for this effect type' : undefined,
            });
        }
    }
    options.push({ value: 63, label: 'Program LFO depth' });
    if (format.model !== 'A3000')
        options.push(
            ...choices(
                [
                    'A/D L pan',
                    'A/D R pan',
                    'A/D L level',
                    'A/D R level',
                    'Control 1 range',
                    'Control 2 range',
                    'Control 3 range',
                    'Control 4 range',
                ],
                64,
            ).map((item) => ({ ...item, extended: true })),
        );
    return options;
}
export function programField(key: string, format: ProgramEditorFormat, values: EditorValues): ProgramField | undefined {
    const domain = format.fields.find((item) => item.key === key.replace(/^assignments\.\d+\./, 'assignments.*.'));
    if (!domain) return;
    const native = format.model === 'A3000',
        suffix = key.split('.').at(-1)!;
    let options: ProgramOption[] | undefined;
    let reason = '';
    if (suffix === 'receive') options = receiveOptions(native);
    if (['mono', 'key_crossfade', 'velocity_crossfade', 'portamento'].includes(suffix))
        options = choices(['=Sample', 'Off', suffix === 'portamento' ? '=Program' : 'On'], -1);
    if (suffix === 'alternate_group')
        options = [
            { value: -1, label: '=Sample' },
            { value: 0, label: 'Off' },
            ...Array.from({ length: 16 }, (_, i) => ({ value: i + 1, label: String(i + 1) })),
        ];
    if (suffix === 'output1' || suffix === 'output2')
        options = outputOptions(suffix === 'output1' ? 1 : 2, native, true);
    if (key === 'portamento.type')
        options = choices(['Rate / Fingered', 'Rate / Full time', 'Time / Fingered', 'Time / Full time']);
    if (
        (key === 'portamento.rate' && Number(values['portamento.type']) >= 2) ||
        (key === 'portamento.time' && Number(values['portamento.type']) < 2)
    )
        reason = 'Inactive for this portamento type; saved value is retained';
    if (key === 'ad.source') options = choices(['2 Mono', 'Stereo', 'L+R Mono']);
    if (key.startsWith('ad.') && key !== 'ad.enabled' && !values['ad.enabled'])
        reason = 'A/D input is disabled; saved value is retained';
    if (key === 'ad.right.pan' && values['ad.source'] !== 0) reason = 'Independent right pan applies to 2 Mono';
    if (key.startsWith('ad.') && suffix === 'destination')
        options = outputOptions(key.includes('output1') ? 1 : 2, native, false);
    if (key === 'lfo.wave')
        options = choices(['Off', 'Sine', 'Saw', 'Triangle', 'Square', 'S/H', ...(!native ? ['StepWave'] : [])]).map(
            (item) => ({ ...item, extended: item.value === 6 }),
        );
    if (key === 'lfo.sync')
        options = choices(['Manual', 'MIDI A', ...(!native ? ['MIDI B'] : [])]).map((item) => ({
            ...item,
            a5000Only: item.value === 2,
        }));
    if (key === 'lfo.cycle')
        options = choices([
            'Eighth note',
            'Quarter note',
            'Dotted quarter',
            'Half note',
            'Whole note',
            '2 whole notes',
            '4 whole notes',
        ]);
    if (key === 'lfo.initial_phase') options = choices(['0 degrees', '90 degrees', '180 degrees', '270 degrees']);
    if (key === 'lfo.reset_channel') options = receiveOptions(native, true);
    if (key === 'lfo.reset_note')
        options = [
            { value: -1, label: 'All' },
            ...Array.from({ length: 128 }, (_, value) => ({ value, label: String(value) })),
        ];
    if (key === 'lfo.tempo' && values['lfo.sync'] !== 0) reason = 'Tempo follows the selected MIDI clock';
    if (key === 'lfo.reset_note' && values['lfo.reset_channel'] === -2) reason = 'LFO reset is off';
    if (key === 'step_wave.step_count')
        options = domain.allowedValues!.map((value) => ({ value, label: String(value) }));
    if (key === 'step_wave.slope') options = choices(['None', 'Rising', 'Falling', 'Both']);
    if (key.startsWith('controllers.') && suffix === 'device')
        options = choices([
            ...Array.from({ length: 121 }, (_, i) => `CC ${String(i).padStart(3, '0')}`),
            'Aftertouch',
            'Pitch Bend',
            'Note Number',
            'Velocity',
            'Program LFO',
            ...(!native ? ['Key-on Random'] : []),
        ]);
    if (key.startsWith('controllers.') && suffix === 'function') options = controllerFunctions(format, values);
    if (key.startsWith('controllers.') && suffix === 'type')
        options = choices(['+Offset', '-/+Offset', '+Offset (-exp)', '+Offset (+exp)']);
    if (key.startsWith('effect_connections.')) {
        options = choices(
            key.endsWith('.1')
                ? connectionLabels
                : connectionLabels.map((label) => label.replace(/[123]/g, (n) => String(Number(n) + 3))),
        );
    }
    if (key.startsWith('effects.') && suffix === 'destination') {
        options = choices(effectDestinations.slice(0, domain.max + 1)).map((item) => ({
            ...item,
            a5000Only: item.value >= 6,
        }));
        if (
            programRoutes(values, native ? 3 : 6).some(
                (edge) => edge.from === Number(key.split('.')[1]) && edge.kind === 'connection',
            )
        )
            reason = 'Output feeds the connected effect; saved destination is retained';
    }
    let label = labels[suffix] ?? suffix;
    if (key.startsWith('effect_connections.')) label = key.endsWith('.1') ? 'Ef1-3 connection' : 'Ef4-6 connection';
    if (key.startsWith('ad.') && key.split('.').length > 2)
        label = `${key.includes('.right.') ? 'Right' : 'Left'} ${key.includes('.output') ? `output ${key.includes('output1') ? '1' : '2'} ` : ''}${label.toLowerCase()}`;
    if (key.startsWith('controllers.')) label = `Control ${key.split('.')[1]} ${label.toLowerCase()}`;
    if (key.startsWith('step_wave.values.')) label = `Step ${suffix}`;
    if (key.startsWith('controller_reset.'))
        label = `${key.includes('.b.') ? 'B' : 'A'}${suffix.padStart(2, '0')} controller reset`;
    if (key.startsWith('note_toggle.'))
        label = `${key.includes('.b.') ? 'B' : 'A'}${suffix.padStart(2, '0')} note toggle`;
    const a5000Only = /^effects\.[456]\.|^effect_connections\.2$|^(controller_reset|note_toggle)\.b\./.test(key);
    const extended = key.startsWith('step_wave.');
    return { ...domain, key, label, options, extended, a5000Only, reason };
}
