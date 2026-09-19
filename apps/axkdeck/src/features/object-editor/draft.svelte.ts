export type EditorValue = number | boolean;
export type EditorValues = Record<string, EditorValue>;

export class EditorDraft {
    values = $state.raw<EditorValues>({});
    private baseline: EditorValues;
    private past = $state.raw<EditorValues[]>([]);
    private future = $state.raw<EditorValues[]>([]);
    private gesture: EditorValues | null = null;

    constructor(values: EditorValues) {
        this.baseline = { ...values };
        this.values = { ...values };
    }
    get changes(): EditorValues {
        return Object.fromEntries(Object.entries(this.values).filter(([key, value]) => this.baseline[key] !== value));
    }
    get dirty(): boolean {
        return Object.keys(this.changes).length > 0;
    }
    get canUndo(): boolean {
        return this.past.length > 0;
    }
    get canRedo(): boolean {
        return this.future.length > 0;
    }
    baselineValue(key: string): EditorValue | undefined {
        return this.baseline[key];
    }
    patch(values: EditorValues): void {
        if (!Object.entries(values).some(([key, value]) => this.values[key] !== value)) return;
        if (!this.gesture) this.past = [...this.past.slice(-99), this.values];
        this.future = [];
        this.values = { ...this.values, ...values };
    }
    set(key: string, value: EditorValue): void {
        if (this.values[key] === value) return;
        if (!this.gesture) this.past = [...this.past.slice(-99), this.values];
        this.future = [];
        this.values = { ...this.values, [key]: value };
    }
    beginGesture(): void {
        if (this.gesture) return;
        this.gesture = this.values;
    }
    endGesture(): void {
        if (this.gesture && Object.keys(this.values).some((key) => this.values[key] !== this.gesture![key]))
            this.past = [...this.past.slice(-99), this.gesture];
        this.gesture = null;
    }
    undo(): void {
        this.endGesture();
        const value = this.past.at(-1);
        if (!value) return;
        this.future = [...this.future, this.values];
        this.past = this.past.slice(0, -1);
        this.values = value;
    }
    redo(): void {
        const value = this.future.at(-1);
        if (!value) return;
        this.past = [...this.past, this.values];
        this.future = this.future.slice(0, -1);
        this.values = value;
    }
    discard(): void {
        this.accept(this.baseline);
    }
    accept(values: EditorValues): void {
        this.baseline = { ...values };
        this.values = { ...values };
        this.past = [];
        this.future = [];
        this.gesture = null;
    }
}
