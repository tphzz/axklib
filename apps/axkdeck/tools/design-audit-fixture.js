import { mount, unmount } from 'svelte';
import '/src/app.css';
import WorkspaceManager from '/src/lib/components/WorkspaceManager.svelte';
import PackageImportDialog from '/src/lib/components/PackageImportDialog.svelte';
import AllocationInspector from '/src/AllocationInspector.svelte';
import WaveCollectionRow from '/src/lib/components/WaveCollectionRow.svelte';
import { ImportCompletion } from '/src/features/import/importCompletion.svelte.ts';
import { JobController } from '/src/features/jobs/actions.ts';
window.__AXKLIB_SERVER__ = { baseUrl: location.origin + '/audit-api', bearerToken: 'synthetic-audit', mode: 'remote' };
const noop = () => {};
let component;
const mode = new URLSearchParams(location.search).get('component');
if (mode === 'completion') {
    window.runPackageCompletionAudit = async () => {
        const result = {
            schemaVersion: '1.0',
            imageId: 'audit',
            sourceMemberId: 'source-1',
            documentId: 'document-1',
            revision: 2,
            planId: 'audit-plan',
            applied: true,
            objectCount: 25,
            targetKind: 'SFS',
            targetSnapshotId: 'audit',
            actions: [],
            allocation: [],
            programAssignmentAdjustments: [],
            programSlotPlacements: [],
        };
        const complete = { jobId: 71, kind: 'images.package_import', status: 'completed', result };
        let refreshCalls = 0,
            starts = 0,
            waits = 0;
        const transport = {
            waitForJob: async () => {
                waits++;
                return complete;
            },
            cancelJob: async () => {},
        };
        const completion = new ImportCompletion(transport, new JobController(transport));
        const accepted = await completion.run(
            async () => {
                starts++;
                return { jobId: 71, kind: 'images.package_import', status: 'queued' };
            },
            async () => {
                refreshCalls++;
            },
        );
        const first = {
            accepted,
            phase: completion.phase,
            message: completion.message,
            locked: completion.locked,
            canDismiss: completion.canDismiss,
            refreshCalls,
        };
        const recovered = await completion.recover();
        return {
            first,
            recovery: {
                recovered,
                phase: completion.phase,
                message: completion.message,
                locked: completion.locked,
                canDismiss: completion.canDismiss,
            },
            starts,
            waits,
            refreshCalls,
            result,
        };
    };
} else if (mode === 'wave') {
    const metrics = (window.waveAudit = { plays: 0, stops: 0, selections: 0, seeks: [] });
    component = mount(WaveCollectionRow, {
        target: document.getElementById('app'),
        props: {
            objectId: 'audit-wave',
            name: 'Audit wave',
            index: 0,
            metadata: '44,100 Hz',
            format: 'PCM16',
            playing: !new URLSearchParams(location.search).has('idle'),
            values: Array.from({ length: 64 }, () => ({ minimum: -0.7, maximum: 0.7 })),
            timeline: {
                sampleRate: 44100,
                storedFrameCount: 44100,
                playbackStartFrame: 0,
                playbackLengthFrames: 44100,
                loopStartFrame: 0,
                loopLengthFrames: 0,
                displayDurationSeconds: 1,
            },
            unavailable: new URLSearchParams(location.search).has('unavailable') ? 'Companion media missing' : '',
            onplay: () => metrics.plays++,
            onstop: () => metrics.stops++,
            onselect: () => metrics.selections++,
            onseek: (ratio) => metrics.seeks.push(ratio),
            onpreview: noop,
            onkeydown: noop,
        },
    });
} else if (mode === 'workspace') {
    const original = {
        id: 'original',
        displayName: 'Original',
        path: '/audit/original',
        writable: false,
        effectiveWritable: false,
        status: 'AVAILABLE',
        issue: null,
    };
    const audit = (window.workspaceAudit = { requests: 0, registered: false, release: null });
    let data;
    const pending = new Promise((resolve) => (audit.release = resolve));
    const originalFetch = window.fetch;
    window.fetch = async (url, options) => {
        if (!String(url).includes('/audit-api/')) return originalFetch(url, options);
        if (String(url).endsWith('/workspaces') && options?.method === 'POST') {
            audit.requests++;
            data = JSON.parse(options.body);
            await pending;
            audit.registered = true;
            return response({
                id: 'added',
                ...data,
                effectiveWritable: data.writable,
                status: 'AVAILABLE',
                issue: null,
            });
        }
        if (String(url).endsWith('/workspaces'))
            return response({
                state: 'READY',
                revision: audit.registered ? 2 : 1,
                workspaces: audit.registered
                    ? [original, { ...original, id: 'added', displayName: data.displayName }]
                    : [original],
                configurationIssue: null,
            });
        if (String(url).endsWith('/roots'))
            return response({ roots: [{ name: 'Audit storage', path: '/audit/storage' }] });
        if (String(url).endsWith('/list'))
            return response({ path: '/audit/storage', parentPath: null, entries: [], nextCursor: null });
        throw Error('Unexpected fixture API ' + url);
    };
    component = mount(WorkspaceManager, {
        target: document.getElementById('app'),
        props: { open: true, onclose: () => unmount(component) },
    });
} else if (mode === 'allocation') {
    const owners = Array.from({ length: 15 }, (_, i) => ({
        objectName: 'Owner ' + i,
        objectType: 'SMPL',
        recordKind: 'OBJECT',
        volumeName: 'Audit Volume',
        sfsId: i + 1,
        claimKind: 'DATA',
        extentIndex: 0,
        objectId: 'owner-' + i,
        categoryName: 'SMPL',
    }));
    const map = {
        imageId: 'audit',
        sourceMemberId: 'source-1',
        partitionName: 'Audit allocation',
        partitionIndex: 0,
        revision: 1,
        partitionStartSector: 0,
        sectorSizeBytes: 512,
        clusterSizeBytes: 4096,
        sectorsPerCluster: 8,
        clusterCount: 65536,
        summary: {
            totalClusters: 65536,
            allocatedClusters: 65536,
            freeClusters: 0,
            largestFreeRunClusters: 0,
            recordCount: 15,
            totalExtentCount: 15,
            fragmentedRecordCount: 0,
            logicalRecordBytes: 16384,
            allocatedBytes: 16384,
            dataSlackBytes: 0,
            bitmapCopyMismatchClusters: 0,
            claimedButFreeClusters: 0,
            usedWithoutClaimClusters: 0,
            conflictingClusters: 65536,
            invalidExtentRecords: 0,
        },
        runs: [
            {
                startCluster: 0,
                clusterCount: 65536,
                allocationKind: 'CONFLICT',
                consistencyFlags: ['MULTIPLE_CLAIMS'],
                owners,
                bitmapCopy1Used: true,
                bitmapCopy2Used: true,
                reconstructedUsed: true,
                byteOffset: 0,
                byteCount: 16384,
                startSector: 0,
                sectorCount: 32,
            },
        ],
    };
    const originalFetch = window.fetch;
    window.fetch = (url, options) =>
        String(url).includes('/audit-api/') ? Promise.resolve(response(map)) : originalFetch(url, options);
    component = mount(AllocationInspector, { target: document.getElementById('app') });
} else {
    const objects = [
        { nodeId: 'bank', objectType: 'SBAC', name: 'Audit Bank', payloadSizeBytes: 4096 },
        ...Array.from({ length: 24 }, (_, i) => ({
            nodeId: 'sample-' + i,
            objectType: 'SBNK',
            name: 'Sample ' + String(i).padStart(2, '0'),
            payloadSizeBytes: 512,
        })),
    ];
    const inspection = {
        schemaVersion: '1.0',
        packageId: 'audit-package',
        packageKind: 'VOLUME',
        requiredExtension: '.axkvol',
        sourceMediaKind: 'SFS',
        valid: true,
        payloadsVerified: true,
        totalPayloadBytes: 16384,
        roots: [{ kind: 'VOLUME', displayName: 'Audit Volume', nodeIds: ['bank'] }],
        objects,
        relationships: objects.slice(1).map((o, i) => ({
            edgeId: 'edge-' + i,
            sourceNodeId: 'bank',
            targetNodeId: o.nodeId,
            role: 'MEMBER',
            ordinal: i,
        })),
        relationshipCount: 24,
        issues: [],
    };
    const conflicts = objects.slice(1).map((o) => ({
        code: 'SFS_NAME_CONFLICT',
        message: 'Destination already contains ' + o.name,
        nodeId: o.nodeId,
        packageId: 'audit-package',
        packageIndex: 0,
        rootIndex: 0,
        partitionIndex: 0,
        groupName: '',
        volumeName: 'TARGET',
        rawGroup: '',
        rawVolume: '',
    }));
    const capacity = {
        target: 'A3000',
        reports: [
            {
                partitionIndex: 0,
                volumeName: 'TARGET',
                profiles: [
                    {
                        target: 'A3000',
                        status: 'FITS',
                        peakBytes: 131072,
                        minimumResidentBytes: 131072,
                        parameterByteLimit: 6291456,
                        peakSlots: 25,
                        sharedObjectSlotLimit: 4096,
                        reasons: [],
                    },
                ],
            },
        ],
        allowed: true,
    };
    if (true) {
        const profile = capacity.reports[0].profiles[0];
        profile.status = 'DOES_NOT_FIT';
        profile.parameterByteLimit = 524288;
        profile.sharedObjectSlotLimit = 1024;
        profile.reasons = [{ code: 'LOAD_ALLOCATION_FAILED', message: 'A clean load cannot allocate Sample 00.' }];
        capacity.allowed = false;
    }
    const plan = {
        capacity,
        schemaVersion: '1.0',
        imageId: 'audit',
        revision: 1,
        planToken: 'audit-plan',
        planId: 'audit-plan',
        expiresInSeconds: 600,
        targetKind: 'SFS',
        targetSnapshotId: 'audit',
        packages: [],
        valid: false,
        warnings: [],
        opaqueSequences: [],
        conflicts,
        actions: objects.slice(1).map((o, i) => ({
            actionId: 'action-' + i,
            packageIndex: 0,
            rootIndex: 0,
            packageId: 'audit-package',
            nodeId: o.nodeId,
            objectType: 'SBNK',
            sourceName: o.name,
            destinationName: o.name,
            partitionIndex: 0,
            groupName: '',
            volumeName: 'TARGET',
            rawGroup: '',
            rawVolume: '',
            actions: ['CONFLICT'],
            canonicalActionId: null,
            targetSfsId: null,
            targetWaveDataReferenceValue: null,
        })),
        programAssignmentAdjustments: [],
        programSlotPlacements: [],
        allocation: [
            { insertedObjectCount: 0, reusedObjectCount: 0, blockedObjectCount: 24, additionalAllocatedBytes: 0 },
        ],
        sfsIndexCapacity: [],
    };
    const transport = {
        waitForJob: async () => {
            throw Error('Audit cannot write');
        },
        cancelJob: async () => {
            throw Error('Audit cannot write');
        },
    };
    const props = {
        completion: new ImportCompletion(transport, new JobController(transport)),
        onrecover: noop,
        targetName: 'TARGET',
        destinationMode: 'existing',
        destinationPartitionIndex: 0,
        destinationVolumeName: 'TARGET',
        partitionOptions: [{ partitionIndex: 0, name: 'Partition 1' }],
        volumeOptions: [{ partitionIndex: 0, name: 'Partition 1', volumeName: 'TARGET', label: 'TARGET' }],
        desktop: false,
        canChangeSource: true,
        sourceName: 'audit.axkvol',
        inspection,
        plan,
        renames: {},
        programSlots: {},
        status: 'ready',
        progress: 1,
        error: '',
        onchooseworkspace: noop,
        onchooselocal: noop,
        onchange: noop,
        ondestinationmode: noop,
        ondestinationvolume: noop,
        ondestinationpartition: noop,
        ondestinationname: noop,
        onrename: noop,
        onprogramslot: noop,
        onprogramstart: noop,
        onopaquesequenceaction: noop,
        onreplan: noop,
        oncancel: () => unmount(component),
        onconfirm: () => {
            throw Error('Audit cannot import');
        },
    };
    component = mount(PackageImportDialog, { target: document.getElementById('app'), props });
}

function response(data) {
    return new Response(JSON.stringify({ data }), { status: 200, headers: { 'content-type': 'application/json' } });
}
