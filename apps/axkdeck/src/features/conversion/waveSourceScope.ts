import { collectPages } from '../../lib/pagination';
import type { DiskTreeItem } from '../../lib/types';
import type { ImageTransport } from '../../lib/transport';

export interface WaveSourceScope {
    kind: 'image' | 'volume';
    label: string;
    locations(signal: AbortSignal): Promise<Map<string, string>>;
}

export function waveSourceScope(
    transport: Pick<ImageTransport, 'objectPage'>,
    sessionId: number,
    item?: DiskTreeItem,
): WaveSourceScope {
    const kind = item?.kind === 'volume' ? 'volume' : 'image';
    const scopeId = kind === 'image' ? undefined : item?.id;
    const partition = item?.partitionIndex === undefined ? '' : `Partition ${item.partitionIndex + 1}`;
    const label = kind === 'volume' ? [partition, item?.name].filter(Boolean).join(' / ') : '';
    return {
        kind,
        label,
        locations: async (signal) => {
            const rows = await collectPages(
                async (offset, limit) => {
                    const page = await transport.objectPage(sessionId, offset, limit, { scopeId, objectType: 'SMPL' });
                    return { items: page.objects, totalCount: page.totalCount };
                },
                { key: (row) => row.key, pageSize: 200, cancelled: () => signal.aborted },
            );
            signal.throwIfAborted();
            const locations = new Map<string, string>(),
                counts = new Map<string, number>();
            for (const row of rows) {
                const label = [
                    row.partitionName || `Partition ${row.partitionIndex + 1}`,
                    row.volumeName,
                    row.directoryEntryName || row.name,
                ]
                    .filter(Boolean)
                    .join(' / ');
                locations.set(row.key, label);
                counts.set(label, (counts.get(label) ?? 0) + 1);
            }
            for (const [key, label] of locations) if (counts.get(label)! > 1) locations.set(key, `${label} · ${key}`);
            return locations;
        },
    };
}
