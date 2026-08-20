import type { DiskTreeItem } from '../../lib/types';
import type { PackageInspection } from '../../lib/transport';

export type PackageImportDestinationMode = 'existing' | 'create';

export interface PackageImportPartitionOption {
    partitionIndex: number;
    name: string;
}

export interface PackageImportVolumeOption extends PackageImportPartitionOption {
    volumeName: string;
    label: string;
}

export interface PackageImportDestinations {
    partitions: PackageImportPartitionOption[];
    volumes: PackageImportVolumeOption[];
    partitionItems: DiskTreeItem[];
    volumeItems: DiskTreeItem[];
}

export type PackageImportDestination =
    | { kind: 'EXISTING_VOLUME'; partitionIndex: number; volumeName: string }
    | { kind: 'CREATE_VOLUME'; partitionIndex: number; volumeName: string };

export function collectPackageImportDestinations(sourceItems: readonly DiskTreeItem[]): PackageImportDestinations {
    const partitionItems: DiskTreeItem[] = [];
    const volumeItems: DiskTreeItem[] = [];
    const visit = (item: DiskTreeItem): void => {
        if (item.kind === 'partition' && item.partitionIndex !== undefined) partitionItems.push(item);
        if (item.kind === 'volume' && item.partitionIndex !== undefined) volumeItems.push(item);
        item.children?.forEach(visit);
    };
    sourceItems.forEach(visit);
    const partitionNames = new Map(partitionItems.map((item) => [item.partitionIndex!, item.name]));
    return {
        partitionItems,
        volumeItems,
        partitions: partitionItems.map((item) => ({ partitionIndex: item.partitionIndex!, name: item.name })),
        volumes: volumeItems.map((item) => {
            const partitionIndex = item.partitionIndex!;
            const name = partitionNames.get(partitionIndex) ?? `Partition ${partitionIndex + 1}`;
            return {
                partitionIndex,
                name,
                volumeName: item.name,
                label: `${name} / ${item.name}`,
            };
        }),
    };
}

export function initialPackageImportDestination(item: DiskTreeItem): {
    mode: PackageImportDestinationMode;
    partitionIndex: number | null;
    volumeName: string;
} | null {
    if (item.kind === 'volume' && item.partitionIndex !== undefined) {
        return { mode: 'existing', partitionIndex: item.partitionIndex, volumeName: item.name };
    }
    if (item.kind === 'partition' && item.partitionIndex !== undefined) {
        return { mode: 'create', partitionIndex: item.partitionIndex, volumeName: '' };
    }
    return null;
}

export function packageImportDestination(
    mode: PackageImportDestinationMode,
    partitionIndex: number | null,
    rawVolumeName: string,
): PackageImportDestination | null {
    if (partitionIndex === null || rawVolumeName.length === 0) return null;
    if (
        mode === 'create' &&
        (rawVolumeName.length > 16 ||
            rawVolumeName.startsWith(' ') ||
            rawVolumeName.endsWith(' ') ||
            Array.from(rawVolumeName).some((character) => {
                const code = character.charCodeAt(0);
                return code < 0x20 || code > 0x7e;
            }))
    ) {
        return null;
    }
    return {
        kind: mode === 'create' ? 'CREATE_VOLUME' : 'EXISTING_VOLUME',
        partitionIndex,
        volumeName: rawVolumeName,
    };
}

export function suggestedPackageVolumeName(sourceName: string, inspection: PackageInspection | null): string {
    const extension = sourceName.split('.').pop()?.toLowerCase() ?? '';
    if (!inspection || (extension !== 'a3k' && extension !== 'axkvol')) return '';
    return (inspection.roots[0]?.displayName ?? '').trim().slice(0, 16);
}
