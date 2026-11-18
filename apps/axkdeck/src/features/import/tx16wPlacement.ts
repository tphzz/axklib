import type { VolumeImportDestination } from '../../lib/transport';
import type { DiskTreeItem } from '../../lib/types';
import { floppyVolumeName } from './floppySelection';
import { importDestination, initialImportDestination, type ImportDestinations } from './packageDestinations';

export type PlacementOptions = Pick<ImportDestinations, 'partitions' | 'volumes'>;

export function suggestedVolume(source: string) {
    return floppyVolumeName('', source.split(' + ')[0], 'file');
}

export function droppedPlacement(item: DiskTreeItem | null, source: string): VolumeImportDestination | null {
    const initial = initialImportDestination(item);
    return (
        initial &&
        importDestination(initial.mode, initial.partitionIndex, initial.volumeName || suggestedVolume(source))
    );
}

export function placementError(
    target: VolumeImportDestination | null,
    { partitions, volumes }: PlacementOptions,
): string {
    if (!target || !partitions.some((p) => p.partitionIndex === target.partitionIndex)) return 'Choose a partition.';
    const exists = volumes.some(
        (v) => v.partitionIndex === target.partitionIndex && v.volumeName === target.volumeName,
    );
    if (target.kind === 'EXISTING_VOLUME') return exists ? '' : 'Choose an existing volume.';
    if (!target.volumeName) return 'Enter a volume name.';
    if (!importDestination('create', target.partitionIndex, target.volumeName))
        return 'Use 1 to 16 printable ASCII characters, without leading or trailing spaces.';
    if (['PRF3', 'sfserrlog', 'sfserram'].includes(target.volumeName)) return 'This name is reserved.';
    return exists ? 'Name already exists in this partition.' : '';
}
