import { describe, expect, it, vi } from 'vitest';
import type { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { ImageTransport, SamplerRelationship } from '../../lib/transport';
import type { SampleStructureItem, WaveDataItem } from '../../lib/types';
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
});
