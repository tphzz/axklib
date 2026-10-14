import { CatalogWorkflow } from '../features/catalog/workflow.svelte';
import { ImageSessionWorkflow } from '../features/image-session/workflow.svelte';
import { documentBindings } from '../features/image-session/documentSelection';
import type { SessionCollaborators } from '../features/image-session/collaborators';
import type { PickerController } from '../features/dialogs/picker';
import type { WorkspaceProps } from '../features/backends/axklib/workspaceProps';
import { documentCapabilities } from '../features/devices/a-series/openedDocument';
import { documentSession, selectedDocument } from '../lib/httpNativeDocuments';
import { imageLocation } from '../lib/httpImageSources';
import type { ImageTransport, OpenedImage } from '../lib/transport';
import type { FilesystemEntry } from '../lib/filesystem';
import { nativeWorkspaceFixture } from './nativeWorkspaceFixture';
import { nativeDocumentFixture, tx16wDocument } from './nativeDocumentFixture';

export function imageScopeFixture(ambiguousCompanion = false) {
    const native = nativeWorkspaceFixture();
    const names = ['HD50_partition-test.hds', 'single-cycle-waveforms.hds', 'tx16w-1.img', 'tx16w-2.img', 'plain.img'];
    const sources = names.map((name, index) => ({
        ...native.sources[0],
        id: `source-${index + 1}`,
        source: { kind: 'FILE' as const, file: { rootId: 'images', relativePath: name } },
        filesystemRootIds: [`source-${index + 1}:root`],
    }));
    const documents = [
        ...[1, 2].map((index) => ({
            ...native.documents[0],
            id: `document-${index}`,
            sourceMemberIds: [`source-${index}`],
        })),
        { ...tx16wDocument, sourceMemberIds: ['source-3', 'source-4'] },
    ];
    if (ambiguousCompanion)
        documents.push({
            ...tx16wDocument,
            id: 'alternative',
            name: 'Alternative native document',
            sourceMemberIds: ['source-4'],
        });
    const summary = { imageId: 'image-1', revision: 1, sources, documents, discovery: native.discovery };
    let session = documentSession(summary);
    let closed = $state(false);
    let blocked = $state(false);
    let decline = $state(false);
    let fail = $state(false);
    let inventoryGate: ((memberId: string) => Promise<void>) | undefined;
    const noop = () => {};
    const entry = (member: string, suffix: string, parentId: string | null, name: string): FilesystemEntry => ({
        id: `${member}:${suffix}`,
        sourceMemberId: member,
        rootId: `${member}:root`,
        parentId,
        ancestorIds: parentId ? [`${member}:root`] : [],
        name,
        path: parentId ? `/${name}` : '/',
        kind: parentId ? 'file' : 'root',
        sizeBytes: parentId ? 512 : null,
        childCount: parentId ? 0 : 1,
        bindings:
            member === 'source-4' && parentId
                ? [{ documentId: tx16wDocument.id, objectId: 'wave-20', contentScopeId: null }]
                : [],
        interpretation: '',
        storage: '',
        issue: '',
        filesystemMetadata: false,
        rawAttributes: '',
        attributes: [],
    });
    const entries = sources.flatMap((member, index) => [
        entry(member.id, 'root', null, `Partition ${index + 1}`),
        entry(member.id, 'file', `${member.id}:root`, `File ${index + 1}`),
    ]);
    function opened(): OpenedImage {
        const document = selectedDocument(session);
        return {
            ...documentCapabilities(document),
            sessionId: 1,
            revision: summary.revision,
            activeSourceMemberId: session.activeSourceMemberId,
            documentId: session.documentId,
            sources,
            documents,
            discovery: summary.discovery,
            companionSources: [],
            floppySet: null,
            tree: [],
            initialVolume: null,
            objects: [],
            objectTotalCount: 0,
            format: document?.format,
            validation: {
                valid: true,
                issueCount: 0,
                errorCount: 0,
                warningCount: 0,
                objectCount: 0,
                relationshipCount: 0,
            },
        };
    }
    const nativeTransport = nativeDocumentFixture();
    const transport = {
        ...nativeTransport,
        nativeObjectDetail: async (id, objectId) => ({
            ...(await nativeTransport.nativeObjectDetail(id, objectId)),
            revision: summary.revision,
            documentId: session.documentId!,
        }),
        storageMode: 'server',
        openImage: async () => opened(),
        selectDocument: async (_id, documentId, memberId) => {
            if (fail) throw new Error('Image selection failed');
            const member = memberId ?? session.activeSourceMemberId;
            session = documentSession(summary, documentId, member);
            return opened();
        },
        refreshImage: async () => opened(),
        closeImage: async () => {
            closed = true;
        },
        keepImageAlive: async () => {},
        validationIssues: async () => [],
        filesystem: async (_id, query = {}) => {
            if (inventoryGate && !query.parentId && !query.rootId && !query.entryId && !query.objectId)
                await inventoryGate(session.activeSourceMemberId);
            const items = entries.filter((item) =>
                query.objectId
                    ? item.bindings.some((binding) => binding.objectId === query.objectId)
                    : query.entryId
                      ? item.id === query.entryId
                      : query.parentId
                        ? item.parentId === query.parentId
                        : query.rootId
                          ? item.rootId === query.rootId && !!item.parentId && item.name.includes(query.query ?? '')
                          : !item.parentId,
            );
            return {
                revision: summary.revision,
                available: true,
                filesystemName: 'sources',
                deviceView: null,
                items: items.slice(query.offset ?? 0, (query.offset ?? 0) + (query.limit ?? 200)),
                totalCount: items.length,
                rootCapabilities: [],
            };
        },
    } satisfies Partial<ImageTransport>;
    const workflow = new ImageSessionWorkflow(transport as ImageTransport, {} as PickerController);
    const catalog = new CatalogWorkflow({
        transport: transport as ImageTransport,
        sessionId: () => workflow.sessionId,
        stopPlayback: async () => {},
        resetPreviews: noop,
        resetCleanup: noop,
        setStatus: noop,
    });
    const audition = {
        laneQueries: { programs: { primary: '', secondary: '', tertiary: '' } },
        state: { status: 'idle' },
        invalidateSession: async () => {},
        stop: async () => {},
    } as unknown as WorkspaceProps['audition'];
    const mutation = {
        volumeAvailable: false,
        partitionAvailable: false,
        objectRenameAvailable: false,
        setCapabilities: noop,
        reset: noop,
    } as unknown as WorkspaceProps['mutation'];
    const disposable = { dispose: noop };
    workflow.connect({
        catalog,
        audition,
        mutation,
        clearExportSelection: noop,
        exports: disposable,
        volumePackages: disposable,
        volumeFloppies: disposable,
        mediaExports: disposable,
        extentRepairs: disposable,
        packageImport: disposable,
        deletion: disposable,
        programGeneration: disposable,
    } as unknown as SessionCollaborators);
    workflow.confirmEditorLeave = async () => !decline;
    workflow.canSwitchDocument = () => !blocked;
    return {
        workflow,
        setInventoryGate(gate?: (memberId: string) => Promise<void>) {
            inventoryGate = gate;
        },
        get closed() {
            return closed;
        },
        get blocked() {
            return blocked;
        },
        set blocked(value: boolean) {
            blocked = value;
        },
        get decline() {
            return decline;
        },
        set decline(value: boolean) {
            decline = value;
        },
        get fail() {
            return fail;
        },
        set fail(value: boolean) {
            fail = value;
        },
        open: () => workflow.open(sources.map((member) => imageLocation(member.source))),
        refresh: async () => {
            summary.revision++;
            await workflow.refresh();
        },
        get props(): WorkspaceProps {
            return {
                ...documentBindings(workflow),
                transport: transport as ImageTransport,
                revision: workflow.revision,
                sessionId: workflow.sessionId,
                imageLocation: workflow.activeLocation,
                imageOpening: workflow.opening,
                isDesktop: false,
                catalog,
                audition,
                mutation,
                workspaceTabs: [{ id: 'programs', label: 'Programs', icon: 'music' }],
                workspaceView: 'programs',
                sourceItems: [],
                selectedSource: workflow.selectedSource,
                selectedVolumeIds: [],
                audioImport: {} as WorkspaceProps['audioImport'],
                sequenceImport: {} as WorkspaceProps['sequenceImport'],
                importAudio: noop,
                importMidi: noop,
                programs: [],
                samples: [],
                sampleBanks: [],
                waveData: [],
                sequences: [],
                bankMembers: [],
                bankMemberWaveData: [],
                sampleWaveData: [],
                activeCollectionObjectId: '',
                inspectorSelection: null,
                editorSelection: null,
                sourceStatus: workflow.status,
                packageSelection: { items: [], anchors: {} },
                objectDeletionAvailable: false,
                waveDataCleanupAvailable: false,
                programGenerationAvailable: false,
                programAssignmentCleanupAvailable: false,
                packageImportAvailable: false,
                packageExportAvailable: false,
                volumePackageExportAvailable: false,
                volumeFloppyExportAvailable: false,
                audioExportAvailable: false,
                sequenceExportAvailable: false,
                mediaConversionAvailable: false,
                allocationInspectionAvailable: false,
                openConnectionSettings: noop,
                openImage: noop,
                createImage: noop,
                closeImage: () => void workflow.close(),
                showImageIntegrity: () => void workflow.showIntegrity(),
                manageLocations: noop,
                selectSource: noop,
                selectSourceForContext: noop,
                imageAction: noop,
                selectWorkspace: noop,
                exportPackage: noop,
                exportAudio: noop,
                exportWav: noop,
                exportMidi: noop,
                deleteObjects: noop,
                cleanupWaveData: noop,
                generatePrograms: noop,
                cleanupProgramAssignments: noop,
                clearSelection: noop,
                selectionChanged: noop,
                selectionLimit: noop,
                setStatus: (message) => workflow.setStatus(message),
            };
        },
    };
}
