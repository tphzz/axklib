import { mappingRectangle } from './keyboardGeometry';
import type { EditableMappingAxes, KeyboardRange, RangeHandle } from './keyboardMapping';

export interface MappingFeedback {
    range: KeyboardRange;
    handle: RangeHandle;
    axes: EditableMappingAxes;
    identity: string;
    pointer: boolean;
}
export function mappingDragText(feedback: MappingFeedback, formatNote: (note: number) => string): string {
    const { range, handle, axes } = feedback;
    const keys = `Keys: ${range.low} (${formatNote(range.low)}) - ${range.high} (${formatNote(range.high)})`;
    const velocity = `Velocity: ${range.velocityLow} - ${range.velocityHigh}`;
    return handle === 'move'
        ? [axes.keys ? keys : '', axes.velocity ? velocity : ''].filter(Boolean).join(' / ')
        : handle.startsWith('velocity')
          ? velocity
          : keys;
}
export function mappingDragPosition(
    feedback: MappingFeedback,
    start: number,
    span: number,
    width: number,
    height: number,
    labelWidth: number,
    labelHeight: number,
) {
    const rectangle = mappingRectangle(feedback.range, start, span);
    const x =
        ((feedback.handle === 'low'
            ? rectangle.left
            : feedback.handle === 'high'
              ? rectangle.left + rectangle.width
              : rectangle.centerX) *
            width) /
        100;
    const y =
        ((feedback.handle === 'velocityHigh'
            ? rectangle.top
            : feedback.handle === 'velocityLow'
              ? rectangle.top + rectangle.height
              : rectangle.centerY) *
            height) /
        100;
    const clamp = (value: number, total: number, size: number) => Math.max(4, Math.min(total - size - 4, value));
    return {
        left: clamp(x - labelWidth / 2, width, labelWidth),
        top: clamp(y >= labelHeight + 14 ? y - labelHeight - 10 : y + 10, height, labelHeight),
    };
}
