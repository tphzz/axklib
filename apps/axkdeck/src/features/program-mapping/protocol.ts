import type { EditableMappingAxes, RangeBoundary, KeyboardRange, KeyboardZone } from '../object-editor/keyboardMapping';

export const mappingRoles = ['program', 'sample', 'bank', 'members'] as const;
export type MappingRole = (typeof mappingRoles)[number];
export interface MappingTarget {
    id: number;
    limits: KeyboardRange | null;
    editableAxes: EditableMappingAxes;
    editable: boolean;
    canUndo: boolean;
    canRedo: boolean;
    canSave: boolean;
    canDiscard: boolean;
    status: string;
}

export interface MappingSnapshot {
    role: MappingRole;
    owner: string;
    context: string;
    version: number;
    editRevision: number;
    imageRevision: number;
    title: string;
    selectionId: number | null;
    selectionLabel: string;
    selections: { value: number; label: string; disabled?: boolean }[];
    targets: MappingTarget[];
    editableAxes: EditableMappingAxes;
    rootEditable: boolean;
    rangeLabel: string;
    overrides: { boundary: RangeBoundary; label: string; inherited: boolean }[];
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
    | { kind: 'select'; selectionId: number }
    | {
          kind: 'move';
          selectionId: number;
          editRevision: number;
          original: KeyboardRange;
          range: KeyboardRange;
          boundaries: RangeBoundary[];
      }
    | { kind: 'note'; clientId: string; sequence: number; note: number; velocity: number; imageRevision: number }
    | { kind: 'release'; clientId: string; sequence: number }
    | { kind: 'lease'; clientId: string; sequence: number }
    | { kind: 'range'; selectionId: number; range: KeyboardRange; boundaries: RangeBoundary[] }
    | { kind: 'root'; selectionId: number; note: number }
    | { kind: 'inherit'; boundary: RangeBoundary }
    | { kind: 'undo' | 'redo' | 'save' | 'discard' | 'recover' };
export interface MappingCommand {
    role: MappingRole;
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
