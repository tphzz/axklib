export function parameterBlockReason(key: string, blocked: readonly string[]): string {
    if (!blocked.includes(key)) return '';
    if (key === 'expand_detune' || key === 'expand_dephase')
        return 'Editing stereo expansion is not supported. The stored value is retained.';
    return 'The stereo channels have different stored values. Editing this shared setting is not supported.';
}
