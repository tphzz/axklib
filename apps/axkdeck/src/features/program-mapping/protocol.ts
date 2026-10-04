import type { KeyboardRange, KeyboardZone } from '../object-editor/keyboardMapping';

export interface MappingSnapshot {
    owner: string;
    context: string;
    version: number;
    title: string;
    assignmentId: number | null;
    assignments: { value: number; label: string }[];
    zones: KeyboardZone[];
    limits: KeyboardRange | null;
    editable: boolean;
    canUndo: boolean;
    canRedo: boolean;
    canSave: boolean;
    canDiscard: boolean;
    recovery: 'Refresh' | 'Check status' | null;
    status: string;
}
export type MappingAction =
    | { kind: 'ready' }
    | { kind: 'select'; assignmentId: number }
    | { kind: 'range'; assignmentId: number; range: KeyboardRange }
    | { kind: 'undo' | 'redo' | 'save' | 'discard' | 'recover' };
export interface MappingCommand {
    requestId: string;
    context: string;
    version: number;
    action: MappingAction;
}
export interface MappingMessage {
    state: MappingSnapshot;
    requestId?: string;
    error?: string;
}
export interface MappingHostAdapter {
    open(): Promise<void>;
    publish(message: MappingMessage): Promise<void>;
    listen(callback: (command: MappingCommand) => void): Promise<() => void>;
}
export function validRange(range: KeyboardRange): boolean {
    return (
        !!range &&
        [range.low, range.high, range.velocityLow, range.velocityHigh].every(
            (value) => Number.isInteger(value) && value >= 0 && value <= 127,
        ) &&
        range.low <= range.high &&
        range.velocityLow <= range.velocityHigh
    );
}
