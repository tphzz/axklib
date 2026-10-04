export interface EditorOption {
    value: number;
    label: string;
    disabled?: boolean;
    reason?: string;
    extended?: boolean;
    a5000Only?: boolean;
}

export function optionHelp(option: EditorOption): string {
    const availability = option.a5000Only
        ? 'Available on A5000 only.'
        : option.extended
          ? 'Requires a4k/a5k format.'
          : '';
    return [option.label, option.reason, availability].filter(Boolean).join(': ');
}
