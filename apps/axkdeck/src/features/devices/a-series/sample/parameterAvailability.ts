import type { SampleEditingSnapshot } from '../../../../lib/objectEditing';
import type { EditorValues } from '../../../object-editor/draft.svelte';

export function parameterBlockReason(key: string, snapshot: SampleEditingSnapshot): string {
    return snapshot.blockedParameterReasons[key] || snapshot.parameterCapabilities[key]?.reason || '';
}

export function parameterInactiveReason(key: string, values: EditorValues): string {
    if (key !== 'portamento_rate' && key !== 'portamento_time') return '';
    const type = values.portamento_type;
    if (type === 0) return 'Portamento is off. The stored value is retained.';
    if (type === 1) return 'The Program supplies portamento when Type is =Pgm. The Sample value is retained.';
    if (key === 'portamento_rate' && (type === 4 || type === 5))
        return 'Time mode uses Portamento Time. The rate is retained for Rate mode.';
    if (key === 'portamento_time' && (type === 2 || type === 3))
        return 'Rate mode uses Portamento Rate. The time is retained for Time mode.';
    return '';
}
