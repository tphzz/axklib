import type { EditorValue, EditorValues } from '../../../object-editor/draft.svelte';

type Delta = Record<string, { before: EditorValue | undefined; after: EditorValue | undefined }>;
export interface DraftAssignment {
    id: number;
    retainOrdinal?: number;
    kind: string;
    name: string;
    targetObjectId: string | null;
}
type StoredAssignment = Omit<DraftAssignment, 'id' | 'retainOrdinal'> & { ordinal: number };
const activeKey = (id: number) => `$assignment.${id}`;

export class ProgramDraft {
    revision = $state(0);
    storedValues = $state<EditorValues>({});
    private baseline: EditorValues = {};
    private changed = $state<EditorValues>({});
    private past = $state.raw<Delta[]>([]);
    private future = $state.raw<Delta[]>([]);
    private gesture: Delta | null = null;
    private gestureFuture: Delta[] = [];
    private definitions = $state.raw<DraftAssignment[]>([]);
    private original: StoredAssignment[] | undefined;
    private nextId = 0;

    constructor(values: EditorValues, assignments?: StoredAssignment[]) {
        this.acceptProgram(values, assignments);
    }
    get values(): EditorValues {
        return this.storedValues;
    }
    get changes(): EditorValues {
        return Object.fromEntries(
            Object.entries(this.changed).filter(([key]) => {
                const active = /^\$assignment\.(\d+)$/.exec(key);
                const row = /^assignments\.(\d+)\./.exec(key);
                if (!active && !row) return true;
                if (!this.original) return true;
                const id = Number((active ?? row)![1]);
                const definition = this.definitions.find((item) => item.id === id);
                return active
                    ? definition?.retainOrdinal !== undefined || this.storedValues[key] === true
                    : this.storedValues[activeKey(id)] === true;
            }),
        );
    }
    get dirty(): boolean {
        return Object.keys(this.changes).length > 0;
    }
    get assignments(): DraftAssignment[] {
        return this.definitions.filter((row) => this.storedValues[activeKey(row.id)] === true);
    }
    get membershipChanged(): boolean {
        return Object.keys(this.changes).some((key) => key.startsWith('$assignment.'));
    }
    addAssignment(target: { objectId: string; kind: string; name: string }, defaults: EditorValues): number {
        const id = this.nextId++;
        this.definitions = [
            ...this.definitions,
            { id, kind: target.kind, name: target.name, targetObjectId: target.objectId },
        ];
        this.patch({
            [activeKey(id)]: true,
            ...Object.fromEntries(Object.entries(defaults).map(([key, value]) => [`assignments.${id}.${key}`, value])),
        });
        return id;
    }
    removeAssignment(id: number): void {
        if (this.assignments.some((row) => row.id === id)) this.set(activeKey(id), false);
    }
    get canUndo(): boolean {
        return this.past.length > 0;
    }
    get canRedo(): boolean {
        return this.future.length > 0;
    }
    get historyLeafCount(): number {
        return this.past.reduce((sum, delta) => sum + Object.keys(delta).length, 0);
    }
    baselineValue(key: string): EditorValue | undefined {
        return this.baseline[key];
    }

    patch(values: EditorValues): void {
        const delta: Delta = {};
        for (const [key, value] of Object.entries(values)) {
            if (this.storedValues[key] === value) continue;
            delta[key] = { before: this.storedValues[key], after: value };
        }
        if (!Object.keys(delta).length) return;
        if (this.gesture) {
            for (const [key, value] of Object.entries(delta)) {
                this.gesture[key] = {
                    before: key in this.gesture ? this.gesture[key]!.before : value.before,
                    after: value.after,
                };
            }
        } else this.past = [...this.past.slice(-99), delta];
        this.future = [];
        this.apply(delta, 'after');
    }
    set(key: string, value: EditorValue): void {
        this.patch({ [key]: value });
    }
    beginGesture(): void {
        if (!this.gesture) {
            this.gesture = {};
            this.gestureFuture = this.future;
        }
    }
    endGesture(): void {
        if (!this.gesture) return;
        const delta = Object.fromEntries(
            Object.entries(this.gesture).filter(([, value]) => value.before !== value.after),
        );
        if (Object.keys(delta).length) this.past = [...this.past.slice(-99), delta];
        this.gesture = null;
    }
    cancelGesture(): void {
        if (!this.gesture) return;
        this.apply(this.gesture, 'before');
        this.future = this.gestureFuture;
        this.gesture = null;
    }
    undo(): void {
        this.endGesture();
        const delta = this.past.at(-1);
        if (!delta) return;
        this.past = this.past.slice(0, -1);
        this.future = [...this.future, delta];
        this.apply(delta, 'before');
    }
    redo(): void {
        this.endGesture();
        const delta = this.future.at(-1);
        if (!delta) return;
        this.future = this.future.slice(0, -1);
        this.past = [...this.past, delta];
        this.apply(delta, 'after');
    }
    private apply(delta: Delta, side: 'before' | 'after'): void {
        for (const [key, leaf] of Object.entries(delta)) {
            const value = leaf[side];
            if (value === undefined) delete this.storedValues[key];
            else this.storedValues[key] = value;
            if (value === this.baseline[key] || value === undefined) delete this.changed[key];
            else this.changed[key] = value;
        }
        this.revision++;
    }
    changeEffectType(slot: number, type: number, defaults: readonly number[]): void {
        if (defaults.length !== 16) throw new Error('An effect reset requires sixteen parameter words');
        if (this.storedValues[`effects.${slot}.type`] === type) return;
        this.patch({
            [`effects.${slot}.type`]: type,
            [`effects.${slot}.reset`]: true,
            ...Object.fromEntries(defaults.map((value, index) => [`effects.${slot}.words.${index}`, value])),
        });
    }
    discard(): void {
        this.acceptProgram(this.baseline, this.original);
    }
    acceptProgram(values: EditorValues, assignments?: StoredAssignment[]): void {
        this.original = assignments;
        this.definitions =
            assignments?.map(({ ordinal, ...row }) => ({ ...row, id: ordinal, retainOrdinal: ordinal })) ?? [];
        this.nextId = Math.max(-1, ...this.definitions.map((row) => row.id)) + 1;
        this.accept({
            ...Object.fromEntries(Object.entries(values).filter(([key]) => !key.startsWith('$assignment.'))),
            ...Object.fromEntries(this.definitions.map((row) => [activeKey(row.id), true])),
        });
    }
    accept(values: EditorValues): void {
        this.baseline = { ...values };
        this.storedValues = { ...values };
        this.changed = {};
        this.past = [];
        this.future = [];
        this.gesture = null;
        this.revision++;
    }
}
