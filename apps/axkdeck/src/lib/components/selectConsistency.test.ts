import { parse } from 'svelte/compiler';
import { describe, expect, it } from 'vitest';

function hasNativeSelect(node: unknown): boolean {
    if (!node || typeof node !== 'object') return false;
    const record = node as Record<string, unknown>;
    if (record.type === 'RegularElement' && record.name === 'select') return true;
    return Object.values(record).some(hasNativeSelect);
}

describe('shared dropdown ownership', () => {
    it('detects a native select rather than similarly named variables or components', () => {
        expect(hasNativeSelect(parse('<select><option>Device</option></select>', { modern: true }))).toBe(true);
        expect(hasNativeSelect(parse('<SelectControl />', { modern: true }))).toBe(false);
    });
    it('routes every production single-choice dropdown through the shared component', () => {
        const sources = import.meta.glob('/src/**/*.svelte', { query: '?raw', import: 'default', eager: true });
        const native = Object.entries(sources)
            .filter(
                ([path, source]) =>
                    !path.startsWith('/src/test/') && hasNativeSelect(parse(String(source), { modern: true })),
            )
            .map(([path]) => path);
        expect(native).toEqual([]);
    });
});
