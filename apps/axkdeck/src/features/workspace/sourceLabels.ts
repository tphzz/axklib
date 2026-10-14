import type { OpenedImage } from '../../lib/transport';

export function sourceLabels(sources: OpenedImage['sources'], deviceLabel = 'Physical device') {
    const items = sources.map((member) => {
        const source = member.source;
        if (source.kind === 'DEVICE') return { id: member.id, name: deviceLabel, path: deviceLabel };
        const reference = source.kind === 'FILE' ? source.file : source.directory;
        const name = reference.relativePath.replaceAll('\\', '/').split('/').filter(Boolean).at(-1) ?? reference.rootId;
        return { id: member.id, name, path: `${reference.rootId}/${reference.relativePath}` };
    });
    const counts = new Map<string, number>();
    for (const item of items) counts.set(item.name, (counts.get(item.name) ?? 0) + 1);
    return items.map((item) => ({ ...item, label: counts.get(item.name)! > 1 ? item.path : item.name }));
}
