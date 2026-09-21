import { EditorDraft, type EditorValues } from '../../../object-editor/draft.svelte';
import type { SampleEditingSnapshot } from '../../../../lib/objectEditing';

export type OverrideUnit = NonNullable<SampleEditingSnapshot['bankOverrides']>['units'][number];
export const overrideKey = (id: number) => `$bankOverride.${id}`;
export function bankValues(values: EditorValues, units: OverrideUnit[]): EditorValues {
    return {
        ...values,
        ...Object.fromEntries(units.map((unit) => [overrideKey(unit.id), unit.activeSelectors.length > 0])),
    };
}

export class BankDraft extends EditorDraft {
    member = $state.raw<EditorValues>({});
    private effective = $derived.by(() => {
        const values = { ...this.member };
        for (const unit of this.units)
            for (const key of unit.keys)
                if (this.isOverridden(key) || !(key in values)) {
                    if (key in this.storedValues) values[key] = this.storedValues[key]!;
                }
        if (this.isOverridden('sample_eq_frequency') && !this.unit('sample_eq_type')) values.sample_eq_type = 0;
        return values;
    });
    units = $state.raw<OverrideUnit[]>([]);
    constructor(values: EditorValues, units: OverrideUnit[]) {
        super(bankValues(values, units));
        this.units = units;
    }
    override get values(): EditorValues {
        return this.effective;
    }
    override get changes(): EditorValues {
        return Object.fromEntries(
            Object.entries(super.changes).filter(([key]) => key.startsWith('$bankOverride.') || this.isOverridden(key)),
        );
    }
    unit(key: string): OverrideUnit | undefined {
        return this.units.find((unit) => unit.keys.includes(key));
    }
    isOverridden(key: string): boolean {
        const unit = this.unit(key);
        return !!unit && this.storedValues[overrideKey(unit.id)] === true;
    }
    sourceDescription(keys: string[]): string {
        const overridden = keys.filter((key) => this.isOverridden(key)).length;
        return overridden === keys.length
            ? 'Bank override'
            : overridden
              ? 'Bank overrides and preview sample values'
              : 'Preview sample values; drag to override';
    }
    override patch(values: EditorValues): void {
        const patch: EditorValues = {};
        for (const [key, value] of Object.entries(values)) {
            const unit = this.unit(key);
            if (!unit) continue;
            if (!this.isOverridden(key) && !patch[overrideKey(unit.id)]) {
                for (const sibling of unit.keys) {
                    const seed = this.member[sibling] ?? this.storedValues[sibling];
                    if (seed !== undefined) patch[sibling] = seed;
                }
                patch[overrideKey(unit.id)] = true;
            }
            patch[key] = value;
        }
        super.patch(patch);
    }
    clearOverride(key: string): void {
        const unit = this.unit(key);
        if (unit) super.patch({ [overrideKey(unit.id)]: false });
    }
    resetUnits(keys: string[]): void {
        const patch: EditorValues = {};
        for (const unit of this.units.filter((unit) => unit.keys.some((key) => keys.includes(key))))
            for (const key of [...unit.keys, overrideKey(unit.id)]) {
                const value = this.baselineValue(key);
                if (value !== undefined) patch[key] = value;
            }
        super.patch(patch);
    }
}
