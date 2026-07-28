import type { ImageSessionExportRoot } from '../../lib/transport';
import type { PackageExportSelection } from '../../lib/types';

export function imageSessionExportRoots(items: readonly PackageExportSelection[]): ImageSessionExportRoot[] {
    return items.map((item) =>
        item.kind === 'VOLUME'
            ? {
                  kind: 'VOLUME',
                  partitionIndex: item.partitionIndex,
                  volumeName: item.volumeName,
              }
            : { kind: item.kind, objectId: item.objectId },
    );
}

export function exportSelectionLabel(items: readonly PackageExportSelection[]): string {
    return items.length === 1 ? items[0]!.name : `${items.length} objects`;
}
