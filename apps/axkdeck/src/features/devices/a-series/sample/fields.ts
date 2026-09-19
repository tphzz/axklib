export interface SampleField {
    key: string;
    label: string;
    min: number;
    max: number;
    options?: { value: number; label: string }[];
    boolean?: boolean;
    help?: string;
    scale?: number;
    unit?: string;
    note?: boolean;
    special?: { value: number; label: string }[];
}
export interface SamplePage {
    id: string;
    label: string;
    fields: SampleField[];
}
export interface SampleTab {
    id: string;
    label: string;
    pages: SamplePage[];
}
const n = (key: string, label: string, min = 0, max = 127, help?: string): SampleField => ({
    key,
    label,
    min,
    max,
    help,
});
const b = (key: string, label: string): SampleField => ({ ...n(key, label, 0, 1), boolean: true });
const choice = (key: string, label: string, labels: string[]): SampleField => ({
    ...n(key, label, 0, labels.length - 1),
    options: labels.map((label, value) => ({ label, value })),
});
const output1 = [
    'Off',
    'StereoOut',
    'Ef1',
    'Ef2',
    'Ef3',
    'AssgnOut L&R',
    'AssgnOut 1&2',
    'AssgnOut 3&4',
    'AssgnOut 5&6',
    'DIG&OPT',
    'Ef4 (A5000)',
    'Ef5 (A5000)',
    'Ef6 (A5000)',
];
const output2 = [
    'Off',
    'AssgnOut L&R',
    'AssgnOut 1&2',
    'AssgnOut 3&4',
    'AssgnOut 5&6',
    'DIG&OPT',
    'StereoOut',
    'Ef1',
    'Ef2',
    'Ef3',
    'Ef4 (A5000)',
    'Ef5 (A5000)',
    'Ef6 (A5000)',
];
const key = (path: string, label: string, original?: number): SampleField => ({
    ...n(path, label),
    note: true,
    ...(original === undefined ? {} : { special: [{ value: original, label: '=Orig' }] }),
});
const scaling = (prefix: 'level' | 'filter'): SampleField[] => [
    key(`${prefix}_scaling_break1`, 'Breakpoint 1'),
    key(`${prefix}_scaling_break2`, 'Breakpoint 2'),
    n(
        `${prefix}_scaling_${prefix === 'level' ? 'level' : 'cutoff'}1`,
        prefix === 'level' ? 'Level 1' : 'Cutoff 1',
        prefix === 'level' ? 0 : -127,
    ),
    n(
        `${prefix}_scaling_${prefix === 'level' ? 'level' : 'cutoff'}2`,
        prefix === 'level' ? 'Level 2' : 'Cutoff 2',
        prefix === 'level' ? 0 : -127,
    ),
];
const envelope = (prefix: 'aeg' | 'feg' | 'peg'): SampleField[] => [
    ...['attack', 'decay', 'release'].map((stage) =>
        n(
            `${prefix}.${stage}_rate`,
            `${stage[0]!.toUpperCase()}${stage.slice(1)} rate`,
            0,
            127,
            'Native envelope rate. Higher values make the stage faster.',
        ),
    ),
    ...(prefix === 'aeg'
        ? [
              n('aeg.sustain_level', 'Sustain level'),
              choice('aeg.attack_mode', 'Attack mode', ['Rate', 'Hold', 'Rate 2']),
          ]
        : ['init', 'attack', 'sustain', 'release'].map((stage) =>
              n(`${prefix}.${stage}_level`, `${stage[0]!.toUpperCase()}${stage.slice(1)} level`, -127),
          )),
    n(`${prefix}.rate_key_scaling`, 'Rate scaling', -7, 7),
    n(`${prefix}.rate_velocity_sensitivity`, 'Velocity to rate', -63, 63),
    ...(prefix !== 'aeg' ? [n(`${prefix}.level_velocity_sensitivity`, 'Velocity to level', -63, 63)] : []),
    ...(prefix === 'feg' ? [n('feg.attack_level_velocity_sensitivity', 'Velocity to attack level', -63, 63)] : []),
    ...(prefix === 'peg' ? [n('peg.range', 'Pitch range', -63, 63)] : []),
];

export const sampleTabs: SampleTab[] = [
    {
        id: 'trim-loop',
        label: 'Trim/Loop',
        pages: [
            {
                id: 'waveform',
                label: 'Waveform',
                fields: [
                    n('playback.start_frame', 'Playback start', 0, 16777215),
                    n('playback.length_frames', 'Playback length', 1, 16777216),
                    n('loop_start_frame', 'Loop start', 0, 16777215),
                    n('loop_length_frames', 'Loop length', 0, 16777216),
                    choice('loop_mode', 'Playback', [
                        'Forward, no loop',
                        'Forward loop',
                        'Loop until release',
                        'Reverse',
                        'Forward one-shot',
                        'Reverse one-shot',
                    ]),
                ],
            },
            {
                id: 'sample-settings',
                label: 'Sample settings',
                fields: [
                    { ...n('loop_tempo_hundredths', 'Tempo', 8000, 15999), scale: 100, unit: 'BPM' },
                    n(
                        'wave_start_velocity_sensitivity',
                        'Velocity to start',
                        -63,
                        63,
                        '0: Fixed start position\nPositive: Higher velocity moves the start forward\nNegative: Higher velocity moves the start backward',
                    ),
                ],
            },
        ],
    },
    {
        id: 'map-out',
        label: 'Map/Out',
        pages: [
            {
                id: 'mix-key',
                label: 'Mix & Key',
                fields: [
                    n('level', 'Level'),
                    {
                        ...n('pan', 'Pan', -63, 63, 'Center: 0\nLeft: negative values\nRight: positive values'),
                        special: [{ value: -64, label: 'Random' }],
                    },
                    key('root_key', 'Original key'),
                    key('key_low', 'Low key', 255),
                    key('key_high', 'High key', 128),
                    b('key_crossfade', 'Key crossfade'),
                    choice('output1_destination', 'Output 1', output1),
                    n('output1_level', 'Output 1 level'),
                    choice('output2_destination', 'Output 2', output2),
                    n('output2_level', 'Output 2 level'),
                ],
            },
            {
                id: 'pitch',
                label: 'Pitch',
                fields: [
                    n('coarse_tune', 'Coarse tune', -64, 63),
                    { ...n('fine_tune_cents', 'Fine tune', -63, 63), unit: 'ct' },
                    b('fixed_pitch', 'Fixed pitch'),
                    n('random_pitch', 'Random pitch', 0, 63),
                    choice('pitch_bend_type', 'Pitch bend type', [
                        'Normal',
                        'Slow',
                        'Slow & Reverse',
                        'Stop',
                        'Stop & Reverse',
                        'Up 2 / Down 3',
                        'Up 2 / Down 4',
                        'Up 2 / Down 5',
                        'Up 2 / Down 12',
                        'Up 3 / Down 2',
                        'Up 3 / Down 4',
                        'Up 3 / Down 5',
                        'Up 3 / Down 12',
                    ]),
                    n('pitch_bend_range', 'Pitch bend range', 0, 24),
                    choice('portamento_type', 'Portamento type', [
                        'Off',
                        '=Pgm',
                        'Rate (fingered)',
                        'Rate (fulltime)',
                        'Time (fingered)',
                        'Time (fulltime)',
                    ]),
                    n('portamento_rate', 'Portamento rate', 1),
                    n('portamento_time', 'Portamento time', 1),
                ],
            },
            { id: 'level-scaling', label: 'Level scaling', fields: scaling('level') },
            {
                id: 'velocity',
                label: 'Expansion & Velocity',
                fields: [
                    n('expand_detune', 'Detune', -7, 7),
                    n('expand_dephase', 'Dephase', -63, 63),
                    n('expand_width', 'Width', -63, 63),
                    n('velocity_low', 'Low velocity'),
                    n('velocity_high', 'High velocity'),
                    n('velocity_low_limit', 'Velocity limit'),
                    n('velocity_offset', 'Velocity offset', -127),
                    n('velocity_sensitivity', 'Velocity sensitivity', -127),
                    n('velocity_xfade_low', 'Low crossfade'),
                    n('velocity_xfade_high', 'High crossfade'),
                ],
            },
        ],
    },
    {
        id: 'filter',
        label: 'Filter',
        pages: [
            {
                id: 'filter',
                label: 'Filter',
                fields: [
                    choice('filter_type', 'Filter type', [
                        'Bypass',
                        'LowPass1',
                        'LowPass2',
                        'HiPass1',
                        'HiPass2',
                        'BandPass',
                        'BandElim',
                        'LowPass3',
                        'Peak1',
                        'Peak2',
                        '2Peaks',
                        '2Dips',
                        'DualLPFs',
                        'LPF+Peak',
                        'DualHPFs',
                        'HPF+Peak',
                        'LPF+HPF',
                    ]),
                    n('filter_cutoff', 'Cutoff'),
                    n('filter_q_width', 'Q / Width', 0, 31),
                    n('filter_gain', 'Gain', -31, 31),
                    n('filter_cutoff_distance', 'Cutoff distance', -63, 63),
                ],
            },
            {
                id: 'sample-eq',
                label: 'Sample EQ',
                fields: [
                    choice('sample_eq_type', 'EQ type', ['Peak/Dip', 'Low shelf', 'High shelf']),
                    n('sample_eq_frequency', 'EQ frequency selection', 4, 58),
                    n('sample_eq_gain_db', 'EQ gain (dB)', -12, 12),
                    { ...n('sample_eq_width_tenths', 'EQ width', 10, 120), scale: 10 },
                ],
            },
            {
                id: 'filter-scaling',
                label: 'Filter scaling',
                fields: [
                    ...scaling('filter'),
                    {
                        ...n('filter_velocity_to_cutoff', 'Velocity to cutoff', -63, 63),
                        special: Array.from({ length: 5 }, (_, i) => ({ value: 64 + i, label: `Random ${i + 1}` })),
                    },
                    {
                        ...n('filter_velocity_to_q_width', 'Velocity to Q / Width', -63, 63),
                        special: Array.from({ length: 5 }, (_, i) => ({ value: 64 + i, label: `Random ${i + 1}` })),
                    },
                ],
            },
        ],
    },
    {
        id: 'eg',
        label: 'EG',
        pages: [
            { id: 'aeg', label: 'Amplitude', fields: envelope('aeg') },
            { id: 'feg', label: 'Filter', fields: envelope('feg') },
            { id: 'peg', label: 'Pitch', fields: envelope('peg') },
        ],
    },
    {
        id: 'lfo',
        label: 'LFO',
        pages: [
            {
                id: 'lfo',
                label: 'LFO',
                fields: [
                    {
                        ...choice('lfo.wave', 'Wave', ['Saw', 'Triangle', 'Square', 'S & H']),
                        help: 'S & H: Sample & Hold. Its speed is controlled by the Program.',
                    },
                    n('lfo.speed', 'Speed', 1, 128),
                    n('lfo.delay_time', 'Delay'),
                    b('lfo.key_on_sync', 'Key-on sync'),
                    n('lfo.pitch_mod_depth', 'Pitch depth'),
                    n('lfo.cutoff_mod_depth', 'Cutoff depth'),
                    n('lfo.amp_mod_depth', 'Amplitude depth'),
                    b('lfo.pitch_mod_phase_invert', 'Invert pitch phase'),
                    b('lfo.cutoff_mod_phase_invert', 'Invert cutoff phase'),
                ],
            },
        ],
    },
    {
        id: 'midi-ctrl',
        label: 'MIDI/CTRL',
        pages: [
            {
                id: 'midi',
                label: 'MIDI',
                fields: [
                    choice('midi_receive_channel', 'Receive channel', [
                        ...Array.from({ length: 16 }, (_, i) => `A${String(i + 1).padStart(2, '0')}`),
                        'Basic channel',
                    ]),
                    b('mono_mode', 'Mono mode'),
                    n('alternate_group', 'Alternate group', 0, 16),
                ],
            },
            {
                id: 'control',
                label: 'Control',
                fields: Array.from({ length: 6 }, (_, i) => [
                    choice(`controls.${i + 1}.device`, 'Controller', [
                        ...Array.from({ length: 121 }, (_, cc) => `CC ${String(cc).padStart(3, '0')}`),
                        'Aftertouch',
                        'Pitch Bend',
                        'Note Number',
                        'Velocity',
                        'Program LFO',
                        'Key-on Random',
                    ]),
                    choice(`controls.${i + 1}.function`, 'Function', [
                        'Off',
                        'Pitch Modulation Depth',
                        'Amplitude Modulation Depth',
                        'Cutoff Modulation Depth',
                        'Cutoff Bias',
                        'Filter Q/Width',
                        'Pan Bias',
                        'Pitch Bias',
                        'Level',
                        'LFO Speed',
                        'LFO Delay',
                        'AEG Attack Rate',
                        'AEG Release Rate',
                        'PEG Attack Rate',
                        'PEG Release Rate',
                        'FEG Attack Rate',
                        'FEG Release Rate',
                        'Pitch Bend',
                        'Start Address',
                        'FEG All Levels',
                        'Cutoff Distance',
                        'Filter Gain',
                        'Portamento Rate/Time',
                        'AEG Decay Rate',
                        'AEG Sustain Level',
                        'FEG Decay Rate',
                        'FEG Initial Level',
                        'FEG Sustain Level',
                        'PEG Decay Rate',
                        'PEG Initial Level',
                        'PEG Sustain Level',
                        ...Array.from({ length: 6 }, (_, c) => `Control ${c + 1} Range`),
                    ]),
                    choice(`controls.${i + 1}.type`, 'Type', [
                        '+Offset',
                        '-/+Offset',
                        '+Offset (-exp)',
                        '+Offset (+exp)',
                    ]),
                    n(`controls.${i + 1}.range`, 'Range', -63, 63),
                ]).flat(),
            },
        ],
    },
];
export const sampleFields = sampleTabs.flatMap((tab) => tab.pages.flatMap((page) => page.fields));
