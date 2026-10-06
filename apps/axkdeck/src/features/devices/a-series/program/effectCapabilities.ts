import type { ProgramEditorFormat } from '../../../../lib/objectEditing';

export function effectParameterExtended(
    format: ProgramEditorFormat,
    native: ProgramEditorFormat | undefined,
    type: number,
    index: number,
) {
    if (format.model === 'A3000' || !native) return false;
    const current = format.effects
        .find((effect) => effect.id === type)
        ?.parameters.find((parameter) => parameter.index === index);
    const earlier = native.effects
        .find((effect) => effect.id === type)
        ?.parameters.find((parameter) => parameter.index === index);
    return !!current?.editable && (!earlier?.editable || current.min < earlier.min || current.max > earlier.max);
}
