import type { SampleEditingSnapshot } from '../../../../lib/objectEditing';
import type { SampleField } from './fields';
import { outputRoutingField } from './outputRouting';

export const sampleFormatContext = Symbol('sample-format');
export type SampleFormatContext = () => SampleEditingSnapshot;

export function parameterDomain(snapshot: SampleEditingSnapshot, key: string) {
    const capability = snapshot.parameterCapabilities[key];
    return snapshot.sampleFormat.format === 'A3000_188' ? capability?.a3000 : capability?.a4000A5000;
}

export function parameterAllowed(snapshot: SampleEditingSnapshot, key: string, value: number | boolean): boolean {
    const domain = parameterDomain(snapshot, key);
    const number = Number(value);
    return !!domain && (number === domain.extraValue || (number >= domain.minimum && number <= domain.maximum));
}

export function formatField(field: SampleField, snapshot?: SampleEditingSnapshot): SampleField {
    if (!snapshot || field.key.startsWith('playback.')) return field;
    const capability = snapshot.parameterCapabilities[field.key];
    const domain = parameterDomain(snapshot, field.key);
    if (!capability) return field;
    const options = [...(field.options ?? [])];
    if (field.key === 'pitch_bend_type' && !options.some((option) => option.value === 13))
        options.push({ value: 13, label: '13 (A3000)' });
    const extended =
        !!capability.a4000A5000 &&
        (!capability.a3000 ||
            capability.a4000A5000.maximum > capability.a3000.maximum ||
            capability.a4000A5000.minimum < capability.a3000.minimum);
    return outputRoutingField(
        {
            ...field,
            min: domain?.minimum ?? field.min,
            max: domain?.maximum ?? field.max,
            extended,
            ...(field.options
                ? {
                      options: options.map((option) => {
                          const native = capability.a3000;
                          const laterOnly =
                              !!capability.a4000A5000 &&
                              (!native || option.value < native.minimum || option.value > native.maximum);
                          const allowed = parameterAllowed(snapshot, field.key, option.value);
                          const a5000 = capability.a5000Minimum !== null && option.value >= capability.a5000Minimum;
                          return {
                              ...option,
                              label: `${option.label}${a5000 && !option.label.includes('(A5000)') ? ' (A5000)' : ''}`,
                              extended: laterOnly,
                              disabled: !allowed,
                              reason: allowed
                                  ? ''
                                  : laterOnly
                                    ? 'Convert explicitly to a4k/a5k format to use this value.'
                                    : 'This value is not supported by the stored Sample format.',
                          };
                      }),
                  }
                : {}),
        },
        snapshot.sampleFormat.format,
    );
}

export function blockedGraphParameters(snapshot: SampleEditingSnapshot): string[] {
    return [
        ...snapshot.blockedParameters,
        ...Object.entries(snapshot.parameterCapabilities)
            .filter(([, capability]) => !capability.editable || !capability.valid)
            .map(([key]) => key),
    ];
}
