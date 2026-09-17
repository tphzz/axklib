import type { FilesystemExportInspection } from '../../lib/filesystemExport';

export interface ExportTreeRow {
    id: string;
    name: string;
    path: string;
    directory: boolean;
    sizeBytes: number;
    depth: number;
    parent: ExportTreeRow | null;
    children: ExportTreeRow[];
}

export type FileTreePreview = {
    totalBytes: number;
    entries: Pick<FilesystemExportInspection['entries'][number], 'relativePath' | 'directory' | 'sizeBytes'>[];
};

export function buildExportTree(inspection: FileTreePreview, name: string): ExportTreeRow {
    const root: ExportTreeRow = {
        id: '[]',
        name,
        path: name,
        directory: true,
        sizeBytes: inspection.totalBytes,
        depth: 0,
        parent: null,
        children: [],
    };
    const nodes = new Map<string, ExportTreeRow>([[root.id, root]]);
    for (const entry of inspection.entries) {
        let parent = root;
        for (let depth = 0; depth < entry.relativePath.length; depth++) {
            const id = JSON.stringify(entry.relativePath.slice(0, depth + 1));
            let node = nodes.get(id);
            if (!node) {
                const name = entry.relativePath[depth];
                node = {
                    id,
                    name,
                    path: `${parent.path}/${name}`,
                    directory: true,
                    sizeBytes: 0,
                    depth: depth + 1,
                    parent,
                    children: [],
                };
                nodes.set(id, node);
                parent.children.push(node);
            }
            if (depth === entry.relativePath.length - 1) {
                node.directory = entry.directory;
                node.sizeBytes = entry.sizeBytes;
            }
            parent = node;
        }
    }
    return root;
}

export function visibleExportRows(root: ExportTreeRow, collapsed: ReadonlySet<string>): ExportTreeRow[] {
    const result: ExportTreeRow[] = [];
    const pending = [root];
    while (pending.length) {
        const row = pending.pop()!;
        result.push(row);
        if (!collapsed.has(row.id)) for (let i = row.children.length - 1; i >= 0; i--) pending.push(row.children[i]);
    }
    return result;
}

export function exportTreePage(rows: ExportTreeRow[], page: number): ExportTreeRow[] {
    const result = rows.slice(page * 100, (page + 1) * 100);
    let parent = result[0]?.parent;
    while (parent) {
        result.unshift(parent);
        parent = parent.parent;
    }
    return result;
}
