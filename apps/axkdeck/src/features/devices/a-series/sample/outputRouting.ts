import type { SampleStorageFormat } from '../../../../lib/objectEditing';
import type { SampleField } from './fields';

export function outputRoutingField(field: SampleField, format: SampleStorageFormat): SampleField {
    const first = field.key === 'output1_destination' || field.key === 'output1_level';
    const second = field.key === 'output2_destination' || field.key === 'output2_level';
    if (!first && !second) return field;
    const native = format === 'A3000_188';
    const group = first ? 'Main output' : 'Assignable output';
    const level = field.key.endsWith('_level');
    return {
        ...field,
        label: native ? `${group}${level ? ' level' : ''}` : field.label,
        extended: false,
        help: native
            ? first
                ? 'Main output sends the Sample to the stereo outputs or Effect 1-3. Use Assignable output for assignable and digital destinations.'
                : 'Assignable output sends the Sample to an assignable output pair or the digital outputs. Use Main output for stereo and Effect 1-3. Outputs 1-6 and digital outputs use the optional AIEB1 I/O expansion board.'
            : 'Each output can send the Sample to stereo, an effect, or an assignable destination. Outputs 1-6 and digital outputs use the optional AIEB1 I/O expansion board. Effects 4-6 require A5000.',
        ...(field.options
            ? {
                  options: field.options.map((option) => {
                      const crossGroup = first
                          ? option.value >= 5 && option.value <= 9
                          : option.value >= 6 && option.value <= 9;
                      const expansion = first
                          ? option.value >= 6 && option.value <= 9
                          : option.value >= 2 && option.value <= 5;
                      let reason = option.reason ?? '';
                      if (option.value >= 10) {
                          reason = native
                              ? 'This effect requires A5000 and a4k/a5k Sample format.'
                              : 'This effect requires A5000.';
                      } else if (native && crossGroup) {
                          reason = `On A3000, select this destination under ${first ? 'Assignable output' : 'Main output'}. Selecting it here requires a4k/a5k format.`;
                      }
                      if (expansion)
                          reason = [reason, 'Uses the optional AIEB1 I/O expansion board.'].filter(Boolean).join(' ');
                      return { ...option, reason };
                  }),
              }
            : {}),
    };
}
