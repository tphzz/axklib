import type { ApiDocumentSummary } from '../../../lib/httpTransportModels';
import type { ContentPage, OpenedImage } from '../../../lib/transport';
import type { DiskTreeItem } from '../../../lib/types';
import { collectPages } from '../../../lib/pagination';

export function documentCapabilities(document: ApiDocumentSummary | null) {
    const operations = document?.formatId === 'yamaha.a-series' ? document.availableOperations : [];
    const has = (operation: string) => operations.includes(operation);
    return {
        volumeMutationsAvailable: has('images.alter.volumes'),
        partitionMutationsAvailable: has('images.alter.partitions'),
        objectRenameAvailable: has('images.alter.objects'),
        objectDeletionAvailable: has('images.alter.objects'),
        waveDataCleanupAvailable: has('images.deletion.orphans.inspect'),
        programGenerationAvailable: has('images.programs.generate.inspect') && has('images.programs.generate'),
        programAssignmentCleanupAvailable:
            has('images.program_assignments.cleanup.inspect') && has('images.program_assignments.cleanup'),
        packageImportAvailable: has('images.package.import'),
        packageExportAvailable: has('images.package.export'),
        volumePackageExportAvailable: has('images.volume_package_export'),
        volumeFloppyExportAvailable: has('images.volume_floppy_export'),
        audioExportAvailable: has('images.audio_export'),
        sequenceExportAvailable: has('images.sequence_export'),
        mediaConversionAvailable: has('images.media_conversion'),
        extentLayoutRepairAvailable: has('images.extent_layout.repair'),
        allocationInspectionAvailable: document?.format === 'sfs' && document.sourceMemberIds.length === 1,
    };
}

export async function aSeriesHierarchy(
    sessionId: number,
    name: string,
    children: (parentId: string, offset: number, limit: number) => Promise<ContentPage>,
): Promise<Pick<OpenedImage, 'tree' | 'initialVolume'>> {
    const roots = await collectPages((offset, limit) => children('', offset, limit), { key: (item) => item.id });
    let initialVolume: DiskTreeItem | null = null;
    let visited = 0;
    const load = async (nodes: DiskTreeItem[], depth: number): Promise<void> => {
        if (depth > 32) throw new Error('Native navigation exceeds its depth limit');
        for (const node of nodes) {
            if (++visited > 16384) throw new Error('Native navigation exceeds its entry limit');
            if (node.kind === 'volume') {
                initialVolume ??= node;
                continue;
            }
            if (!node.childCount) continue;
            node.children = await collectPages((offset, limit) => children(node.id, offset, limit), {
                key: (item) => item.id,
            });
            await load(node.children, depth + 1);
        }
    };
    await load(roots, 0);
    return {
        tree: [{ id: `session:${sessionId}`, name, kind: 'disk', children: roots, childCount: roots.length }],
        initialVolume,
    };
}
