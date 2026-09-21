import type { OverrideUnit } from '../features/devices/a-series/bank/draft.svelte';

export function bankEditorUnits(native: boolean, enabled: Set<number>): OverrideUnit[] {
    const groups: Record<number, string[]> = {
        3: ['midi_receive_channel'],
        4: ['pitch_bend_type'],
        5: ['pitch_bend_range'],
        9: ['coarse_tune'],
        19: ['wave_start_velocity_sensitivity'],
        21: ['filter_type'],
        22: ['filter_cutoff'],
        23: ['filter_q_width'],
        24: ['filter_scaling_break1', 'filter_scaling_break2'],
        25: ['filter_scaling_cutoff1', 'filter_scaling_cutoff2'],
        26: ['filter_velocity_to_cutoff'],
        27: ['filter_velocity_to_q_width'],
        28: ['fixed_pitch'],
        29: ['expand_detune'],
        30: ['expand_dephase'],
        31: ['expand_width'],
        32: ['random_pitch'],
        33: ['level'],
        34: ['pan'],
        35: ['velocity_low_limit'],
        36: ['velocity_offset'],
        37: ['velocity_high'],
        38: ['velocity_low'],
        39: ['level_scaling_break1', 'level_scaling_break2'],
        40: ['level_scaling_level1', 'level_scaling_level2'],
        41: ['velocity_sensitivity'],
        42: ['portamento_type'],
        43: ['mono_mode'],
        44: ['key_crossfade'],
        48: ['alternate_group'],
        49: ['sample_eq_frequency', 'sample_eq_gain_db', 'sample_eq_width_tenths'],
        52: ['filter_cutoff_distance'],
        53: ['feg.attack_rate', 'feg.decay_rate', 'feg.release_rate'],
        54: ['feg.init_level', 'feg.attack_level', 'feg.sustain_level', 'feg.release_level'],
        55: ['feg.rate_key_scaling'],
        56: ['feg.rate_velocity_sensitivity'],
        57: ['feg.attack_level_velocity_sensitivity'],
        58: ['feg.level_velocity_sensitivity'],
        59: ['peg.attack_rate', 'peg.decay_rate', 'peg.release_rate'],
        60: ['peg.init_level', 'peg.attack_level', 'peg.sustain_level', 'peg.release_level'],
        61: ['peg.rate_key_scaling'],
        62: ['peg.rate_velocity_sensitivity'],
        63: ['peg.level_velocity_sensitivity'],
        64: ['peg.range'],
        65: ['aeg.attack_rate', 'aeg.decay_rate', 'aeg.release_rate'],
        66: ['aeg.sustain_level'],
        67: ['aeg.rate_key_scaling'],
        68: ['aeg.rate_velocity_sensitivity'],
        69: ['aeg.attack_mode'],
        70: ['lfo.wave'],
        71: ['lfo.speed'],
        72: ['lfo.delay_time'],
        73: ['lfo.key_on_sync'],
        74: ['lfo.pitch_mod_phase_invert'],
        75: ['lfo.cutoff_mod_phase_invert'],
        76: ['lfo.cutoff_mod_depth'],
        77: ['lfo.pitch_mod_depth'],
        78: ['lfo.amp_mod_depth'],
        79: ['output1_destination'],
        80: ['output1_level'],
        81: ['output2_destination'],
        82: ['output2_level'],
        83: Array.from({ length: 6 }, (_, i) =>
            ['device', 'function', 'type', 'range'].map((key) => `controls.${i + 1}.${key}`),
        ).flat(),
        84: ['filter_gain'],
    };
    if (native) groups[45] = ['velocity_crossfade'];
    else {
        groups[46] = ['velocity_xfade_low'];
        groups[47] = ['velocity_xfade_high'];
        groups[49]!.push('sample_eq_type');
        groups[87] = ['portamento_rate'];
        groups[88] = ['portamento_time'];
    }
    return Object.entries(groups).map(([id, keys]) => {
        const selectors = Number(id) === 49 ? (native ? [49, 50, 51] : [49, 50, 51, 85]) : [Number(id)];
        return { id: Number(id), keys, selectors, activeSelectors: enabled.has(Number(id)) ? selectors : [] };
    });
}
