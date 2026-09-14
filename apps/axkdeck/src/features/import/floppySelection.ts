import type { FloppyObject } from '../../lib/floppyImport';

export function floppySelection(objects: readonly FloppyObject[], selected: readonly string[]) {
    const available = new Map(objects.filter((item) => !item.exclusionReason).map((item) => [item.objectKey, item]));
    const roots = new Set(selected.filter((key) => available.has(key)));
    const included = new Set(roots);
    const required = new Set<string>();
    const pending = [...roots];
    const visited = new Set<string>();
    while (pending.length) {
        const key = pending.pop()!;
        if (visited.has(key)) continue;
        visited.add(key);
        for (const dependency of available.get(key)?.requiredObjectKeys ?? []) {
            required.add(dependency);
            included.add(dependency);
            pending.push(dependency);
        }
    }
    return { included, required };
}

export function floppyVolumeName(label: string, filename: string): string {
    const clean = (value: string) =>
        value
            .replace(/[^\x20-\x7e]/g, '')
            .trim()
            .slice(0, 16)
            .trim();
    return clean(label) || clean(filename.replace(/\.(img|ima)$/i, '')) || 'Imported';
}
