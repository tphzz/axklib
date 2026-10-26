export interface SelectOption<Value extends string | number = string | number> {
    value: Value;
    key?: string | number;
    label: string;
    disabled?: boolean;
    title?: string;
}
