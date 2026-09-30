<script lang="ts">
    import VolumeInspector from '../lib/components/VolumeInspector.svelte';
    import { provideVolumeCapacity } from '../lib/volumeCapacity.svelte';
    import { provideInspectorPanels } from '../lib/inspectorPanels.svelte';
    import type { VolumeCapacityInspection, VolumeCapacityReport } from '../lib/volumeInspections';
    import { volumeCapacityFixture } from './volumeCapacityFixture';

    let {
        report = volumeCapacityFixture(),
        revision = 1,
        volumeId = 'volume',
        pending,
        error = '',
    }: {
        report?: VolumeCapacityReport;
        revision?: number;
        volumeId?: string;
        pending?: Promise<VolumeCapacityInspection>;
        error?: string;
    } = $props();
    provideInspectorPanels();
    provideVolumeCapacity(() => ({
        sessionId: 1,
        revision,
        enabled: true,
        transport: {
            inspectVolumeCapacity: async (_session, contentScopeId) => {
                if (error) throw new Error(error);
                return pending ?? { imageId: 'image', revision, contentScopeId, report };
            },
        },
    }));
    const item = $derived({
        id: volumeId,
        name: 'Capacity Test',
        kind: 'volume' as const,
        childCount: 0,
        partitionIndex: 0,
    });
</script>

<VolumeInspector {item} />
