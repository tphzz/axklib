import { fireEvent, render, waitFor, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { CatalogWorkflow } from '../../catalog/workflow.svelte';
import type { ImageTransport } from '../../../lib/transport';
import type { DiskTreeItem, WorkspaceView } from '../../../lib/types';
import { inspectorRelationshipFixture } from '../../../test/inspectorRelationshipFixture';
import Workspace from './Workspace.svelte';
import type { WorkspaceProps } from './workspaceProps';

function setup() {
    const selection = inspectorRelationshipFixture('program');
    if (selection?.kind !== 'program') throw new Error('Expected Program fixture');
    const transport = { storageMode: 'server' } as ImageTransport;
    const catalog = new CatalogWorkflow({
        transport,
        sessionId: () => 1,
        stopPlayback: async () => {},
        resetPreviews: () => {},
        resetCleanup: () => {},
        setStatus: () => {},
    });
    catalog.activeVolumeId = 'volume';
    catalog.programs = [selection.program];
    catalog.systemProgramContexts = {
        partitionIndex: 0,
        message: '',
        files: [
            {
                fileKind: 'SYSTEM2',
                availability: 'AVAILABLE',
                storageRevision: 0,
                savedProgramMode: 'SINGLE',
                basicReceive: { port: 'A', channel: 1, display: 'A01' },
                omni: false,
                programChangeEnabled: true,
                parts: [
                    {
                        partNumber: 1,
                        partLabel: 'A01',
                        midi: { port: 'A', channel: 1, display: 'A01' },
                        programNumber: 1,
                        master: true,
                    },
                    {
                        partNumber: 2,
                        partLabel: 'A02',
                        midi: { port: 'A', channel: 2, display: 'A02' },
                        programNumber: 2,
                        master: false,
                    },
                ],
            },
        ],
    };
    const selectProgram = vi.fn((program: typeof selection.program) => {
        catalog.selectedProgramId = program.objectId;
        catalog.inspectorObjectId = program.objectId;
        catalog.editorObjectIds.programs = program.objectId;
    });
    const navigateToObject = vi.fn(async (): Promise<WorkspaceView | null> => 'programs');
    const props: WorkspaceProps = {
        transport,
        isDesktop: false,
        workspaceTabs: [{ id: 'programs', label: 'Programs', icon: 'music' }],
        workspaceView: 'programs',
        imageLocation: null,
        sourceItems: [],
        selectedSource: { id: 'volume', kind: 'volume', partitionIndex: 0 } as DiskTreeItem,
        selectedVolumeIds: [],
        imageOpening: false,
        sessionId: 1,
        catalog,
        audition: {
            laneQueries: { programs: { primary: '', secondary: '', tertiary: '' } },
            state: { status: 'idle' },
            selectProgram,
            navigateToObject,
        } as unknown as WorkspaceProps['audition'],
        mutation: {
            volumeAvailable: false,
            partitionAvailable: false,
            objectRenameAvailable: false,
        } as WorkspaceProps['mutation'],
        audioImport: {} as WorkspaceProps['audioImport'],
        sequenceImport: {} as WorkspaceProps['sequenceImport'],
        importAudio: vi.fn(),
        importMidi: vi.fn(),
        programs: catalog.programs,
        sampleBanks: [],
        samples: [],
        waveData: [],
        sequences: [],
        bankMembers: [],
        bankMemberWaveData: [],
        sampleWaveData: [],
        activeCollectionObjectId: '',
        inspectorSelection: inspectorRelationshipFixture('sample'),
        editorSelection: selection,
        sourceStatus: '',
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
        openConnectionSettings: vi.fn(),
        openImage: vi.fn(),
        createImage: vi.fn(),
        closeImage: vi.fn(),
        showImageIntegrity: vi.fn(),
        manageLocations: vi.fn(),
        selectSource: vi.fn(),
        selectSourceForContext: vi.fn(),
        imageAction: vi.fn(),
        selectWorkspace: vi.fn(),
        exportPackage: vi.fn(),
        exportAudio: vi.fn(),
        exportWav: vi.fn(),
        exportMidi: vi.fn(),
        deleteObjects: vi.fn(),
        cleanupWaveData: vi.fn(),
        generatePrograms: vi.fn(),
        cleanupProgramAssignments: vi.fn(),
        clearSelection: vi.fn(),
        selectionChanged: vi.fn(),
        selectionLimit: vi.fn(),
        setStatus: vi.fn(),
    };
    return { props, catalog, selectProgram, navigateToObject, view: render(Workspace, props) };
}

describe('Program lower-zone selection', () => {
    it('opens the lower zone when a Single Program is explicitly selected', async () => {
        const { view, selectProgram } = setup();
        const toggle = view.getByRole('button', { name: 'Editor panel' });
        expect(toggle.getAttribute('aria-pressed')).toBe('false');
        await fireEvent.click(view.container.querySelector<HTMLButtonElement>('.program-row')!);

        expect(selectProgram).toHaveBeenCalledOnce();
        expect(toggle.getAttribute('aria-pressed')).toBe('true');
        expect(view.getByRole('tablist', { name: 'Program editor' })).toBeTruthy();
    });

    it('opens the lower zone for a populated Multi Part but not an empty part', async () => {
        const { view, selectProgram } = setup();
        const toggle = view.getByRole('button', { name: 'Editor panel' });
        await fireEvent.click(view.getByRole('button', { name: 'Multi Part view' }));
        await fireEvent.click(view.getByRole('button', { name: /Part A02/ }));
        expect(toggle.getAttribute('aria-pressed')).toBe('false');
        expect(selectProgram).not.toHaveBeenCalled();

        await fireEvent.click(view.getByRole('button', { name: /Part A01/ }));
        expect(selectProgram).toHaveBeenCalledOnce();
        expect(toggle.getAttribute('aria-pressed')).toBe('true');
        expect(view.getByRole('tablist', { name: 'Program editor' })).toBeTruthy();
    });

    it('opens the lower zone after inspector relationship navigation resolves to a Program', async () => {
        const { view, navigateToObject } = setup();
        const inspector = within(view.getByRole('complementary', { name: 'Object inspector' }));
        await fireEvent.click(inspector.getByRole('button', { name: 'Relationships' }));
        await fireEvent.click(inspector.getByRole('button', { name: /001: NoizLoop/ }));

        await waitFor(() => expect(navigateToObject).toHaveBeenCalledOnce());
        await waitFor(() =>
            expect(view.getByRole('button', { name: 'Editor panel' }).getAttribute('aria-pressed')).toBe('true'),
        );
        expect(view.getByRole('tablist', { name: 'Program editor' })).toBeTruthy();
    });

    it('does not reopen a manually closed lower zone when selection metadata refreshes in the background', async () => {
        const { view, props, catalog } = setup();
        const toggle = view.getByRole('button', { name: 'Editor panel' });
        await fireEvent.click(toggle);
        expect(toggle.getAttribute('aria-pressed')).toBe('true');
        await fireEvent.click(toggle);
        expect(toggle.getAttribute('aria-pressed')).toBe('false');

        catalog.selectedProgramId = props.programs[0]!.objectId;
        catalog.editorObjectIds.programs = catalog.selectedProgramId;
        catalog.systemProgramContexts = { ...catalog.systemProgramContexts! };
        await view.rerender({
            programs: [...props.programs],
            editorSelection: inspectorRelationshipFixture('program'),
            activeCollectionObjectId: catalog.selectedProgramId,
        });

        expect(toggle.getAttribute('aria-pressed')).toBe('false');
        expect(view.queryByRole('tablist', { name: 'Program editor' })).toBeNull();
    });
});
