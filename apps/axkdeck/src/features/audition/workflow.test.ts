import { describe, expect, it, vi } from 'vitest';
import { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { ImageTransport, SamplerRelationship } from '../../lib/transport';
import type { SampleStructureItem, WaveDataItem, WorkspaceView } from '../../lib/types';
import { AuditionWorkflow } from './workflow.svelte';

function relationship(
    id: string,
    sourceObjectId: string,
    relationshipType: string,
    targetObjectId: string,
): SamplerRelationship {
    return {
        id,
        sourceObjectId,
        targetObjectId,
        candidateObjectIds: [],
        relationshipType,
        quality: 'KNOWN',
        basis: 'test',
        notes: [],
        assignmentName: '',
        assignmentState: '',
        receiveChannelDisplay: '',
    };
}

describe('AuditionWorkflow auditionability indexes', () => {
    it('reuses one indexed Sample Bank result across repeated row reads', () => {
        const sample = { objectId: 'sample-1' } as SampleStructureItem;
        const bank = { objectId: 'bank-1' } as SampleStructureItem;
        const waveData = { objectKey: 'wave-1' } as WaveDataItem;
        const membersForBank = vi.fn(() => [sample]);
        const catalog = {
            relationships: [
                relationship('bank-member', bank.objectId, 'SBAC_SLOT_TO_SBNK', sample.objectId),
                relationship('sample-wave', sample.objectId, 'SBNK_LEFT_MEMBER_TO_SMPL', waveData.objectKey),
            ],
            waveData: [waveData],
            sampleBanks: [bank],
            samples: [sample],
            membersForBank,
        } as unknown as CatalogWorkflow;
        const workflow = new AuditionWorkflow({
            transport: {} as ImageTransport,
            catalog,
            sessionId: () => 1,
            workspaceView: () => 'sample-banks',
            setWorkspaceView: () => undefined,
            setInspectorOpen: () => undefined,
            setStatus: () => undefined,
            requestCompanionDisks: () => undefined,
        });

        const first = workflow.auditionableSampleBankObjectIds;
        const second = workflow.auditionableSampleBankObjectIds;

        expect(first).toEqual(new Set([bank.objectId]));
        expect(second).toBe(first);
        expect(membersForBank).not.toHaveBeenCalled();
    });

    it('clears assigned Sample context and stops its playback when the standalone filter is enabled', async () => {
        const sample = { objectId: 'sample-1', sampleBankObjectIds: ['bank-1'] } as SampleStructureItem;
        const waveData = { objectKey: 'wave-1' } as WaveDataItem;
        const clearSampleSelection = vi.fn();
        const catalog = {
            relationships: [],
            waveData: [waveData],
            sampleBanks: [],
            samples: [sample],
            selectedSampleId: sample.objectId,
            waveDataForSample: vi.fn(() => [waveData]),
            clearSampleSelection,
        } as unknown as CatalogWorkflow;
        const workflow = new AuditionWorkflow({
            transport: {} as ImageTransport,
            catalog,
            sessionId: () => 1,
            workspaceView: () => 'samples',
            setWorkspaceView: () => undefined,
            setInspectorOpen: () => undefined,
            setStatus: () => undefined,
            requestCompanionDisks: () => undefined,
        });
        workflow.showOnlyStandaloneSamples = false;
        workflow.state = { objectId: waveData.objectKey, status: 'playing', playheadFrame: 0 };

        workflow.updateShowOnlyStandaloneSamples(true);
        await vi.waitFor(() => expect(workflow.state.status).toBe('idle'));

        expect(workflow.showOnlyStandaloneSamples).toBe(true);
        expect(clearSampleSelection).toHaveBeenCalledOnce();
    });

    it('stops playback and exposes an assigned Sample before relationship navigation', async () => {
        const sample = {
            objectId: 'sample-1',
            sampleBankObjectIds: ['bank-1'],
        } as SampleStructureItem;
        const catalog = {
            relationships: [],
            programs: [],
            sequences: [],
            sampleBanks: [],
            samples: [sample],
            waveData: [],
            selectedSampleId: '',
            selectedSampleWaveDataId: '',
            editorObjectIds: { programs: '', sequences: '', 'sample-banks': '', samples: '', 'wave-data': '' },
            inspectorObjectId: '',
            samplePreviewStates: {},
        } as unknown as CatalogWorkflow;
        let workspaceView: 'programs' | 'samples' = 'programs';
        const setWorkspaceView = vi.fn((view: WorkspaceView) => {
            if (view === 'programs' || view === 'samples') workspaceView = view;
        });
        const setInspectorOpen = vi.fn();
        const workflow = new AuditionWorkflow({
            transport: {} as ImageTransport,
            catalog,
            sessionId: () => null,
            workspaceView: () => workspaceView,
            setWorkspaceView,
            setInspectorOpen,
            setStatus: () => undefined,
            requestCompanionDisks: () => undefined,
        });
        const stop = vi.spyOn(workflow, 'stop').mockResolvedValue();
        workflow.showOnlyStandaloneSamples = true;
        workflow.laneQueries.samples.primary = 'hidden';

        const view = await workflow.navigateToObject(sample.objectId);

        expect(view).toBe('samples');
        expect(stop).toHaveBeenCalledOnce();
        expect(setWorkspaceView).toHaveBeenCalledWith('samples');
        expect(workflow.showOnlyStandaloneSamples).toBe(false);
        expect(workflow.laneQueries.samples.primary).toBe('');
        expect(catalog.selectedSampleId).toBe(sample.objectId);
        expect(catalog.editorObjectIds.samples).toBe(sample.objectId);
        expect(catalog.inspectorObjectId).toBe(sample.objectId);
        expect(setInspectorOpen).toHaveBeenCalledWith(true);
    });

    it('does not navigate an object outside the active volume graph', async () => {
        const catalog = {
            relationships: [],
            programs: [],
            sequences: [],
            sampleBanks: [],
            samples: [],
            waveData: [],
        } as unknown as CatalogWorkflow;
        const workflow = new AuditionWorkflow({
            transport: {} as ImageTransport,
            catalog,
            sessionId: () => null,
            workspaceView: () => 'programs',
            setWorkspaceView: () => undefined,
            setInspectorOpen: () => undefined,
            setStatus: () => undefined,
            requestCompanionDisks: () => undefined,
        });
        const stop = vi.spyOn(workflow, 'stop').mockResolvedValue();

        expect(await workflow.navigateToObject('missing')).toBeNull();
        expect(stop).not.toHaveBeenCalled();
    });
});

describe('CatalogWorkflow Sample context', () => {
    it('clears only the Sample lane and its dependent inspector context', () => {
        const catalog = new CatalogWorkflow({
            transport: {} as ImageTransport,
            sessionId: () => 1,
            stopPlayback: () => Promise.resolve(),
            resetPreviews: () => undefined,
            resetCleanup: () => undefined,
            setStatus: () => undefined,
        });
        catalog.selectedSampleId = 'sample-1';
        catalog.selectedSampleWaveDataId = 'wave-1';
        catalog.inspectorObjectId = 'wave-1';
        catalog.editorObjectIds = { ...catalog.editorObjectIds, programs: 'program-1', samples: 'sample-1' };

        catalog.clearSampleSelection();

        expect(catalog.selectedSampleId).toBe('');
        expect(catalog.selectedSampleWaveDataId).toBe('');
        expect(catalog.inspectorObjectId).toBe('');
        expect(catalog.editorObjectIds.samples).toBe('');
        expect(catalog.editorObjectIds.programs).toBe('program-1');
    });
});
