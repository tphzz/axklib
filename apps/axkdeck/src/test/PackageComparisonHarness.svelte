<script lang="ts">
    import PackageBatchImportDialog from '../lib/components/PackageBatchImportDialog.svelte';
    import { floppyDialogFixture } from './floppyDialogFixture';
    import { serverFileLocation } from '../lib/storageLocations';
    import type { BatchPackageItem } from '../features/import/packageBatchTypes';
    const workflow = floppyDialogFixture();
    const noop = () => {};
    const items: BatchPackageItem[] = Array.from({ length: 8 }, (_, index) => ({
        id: `item-${index}`,
        selected: true,
        source: serverFileLocation({ rootId: 'workspace', relativePath: `Volume ${index + 1}.axkvol` }),
        sourceName: `Volume ${index + 1}.axkvol`,
        upload: null,
        localPath: null,
        inspection: {
            schemaVersion: '1.0',
            packageId: `package-${index}`,
            packageKind: 'VOLUME',
            requiredExtension: '.axkvol',
            sourceMediaKind: 'SFS',
            valid: true,
            payloadsVerified: true,
            totalPayloadBytes: 4096,
            roots: [{ kind: 'VOLUME', displayName: `Volume ${index + 1}`, nodeIds: [] }],
            objects: [],
            relationships: [],
            relationshipCount: 0,
            issues: [],
        },
    }));
    const plan = workflow.request!.plan!;
    plan.packages = items.map((item, packageIndex) => ({
        packageIndex,
        packageId: item.inspection.packageId,
        sourceVolumeName: item.sourceName,
        destinationVolumeName: 'New volume',
        objectCount: 15,
        payloadBytes: 4096,
        objectCounts: { programs: 1, sampleBanks: 1, samples: 5, waveData: 8, sequences: 0 },
    }));
</script>

<PackageBatchImportDialog
    completion={workflow.completion}
    onrecover={noop}
    desktop={true}
    canChangeSources={true}
    {items}
    {plan}
    destinationStrategy="shared"
    destinationMode="create"
    destinationPartitionIndex={0}
    destinationVolumeName="New volume"
    partitionOptions={workflow.destinations().partitions}
    volumeOptions={workflow.destinations().volumes}
    separateVolumesAvailable={true}
    volumeNames={{}}
    renames={{}}
    programSlots={{}}
    opaqueSequenceActions={{}}
    hasUnvalidatedChanges={false}
    status="ready"
    completedFiles={8}
    totalFiles={8}
    progress={1}
    error=""
    onchooseworkspace={noop}
    onchooselocal={noop}
    ondestinationstrategy={noop}
    ondestinationmode={noop}
    ondestinationvolume={noop}
    ondestinationpartition={noop}
    ondestinationname={noop}
    onrenamevolume={noop}
    onrename={noop}
    onprogramslot={noop}
    onprogramstart={noop}
    ontoggleselected={noop}
    ontoggleall={noop}
    onopaquesequenceaction={noop}
    onreplan={async () => {}}
    oncancel={noop}
    onconfirm={noop}
/>
