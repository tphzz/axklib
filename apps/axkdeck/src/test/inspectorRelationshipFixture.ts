import { inspectorRelationshipGroups } from '../lib/inspectorRelationships';
import type { SamplerObject, SamplerRelationship } from '../lib/transport';
import type { InspectorSelection, SampleStructureItem, WaveDataItem } from '../lib/types';

function object(objectType: string, name: string): SamplerObject {
    return {
        key: `${objectType}-${name}`,
        objectType,
        name,
        partitionIndex: 0,
        partitionName: 'PARTITION 1',
        volumeName: 'DEMO',
        categoryName: objectType,
        objectEncoding: 'current',
        directoryEntryName: name,
        sfsId: 0,
        storedSizeBytes: 356,
        sizeWithDependenciesBytes: 158720,
        sampleRate: 44100,
        rootKey: 60,
        storedFrameCount: 44100,
        waveStartFrame: 0,
        waveLengthFrames: 44100,
        storageState: 'COMPLETE',
        sampleWidthBytes: 2,
    };
}

export function inspectorRelationshipFixture(kind: string): InspectorSelection {
    const programObject = object('PROG', '001: NoizLoop');
    const bankObject = object('SBAC', 'DRUMS');
    const samples: SampleStructureItem[] = Array.from({ length: 24 }, (_, index) => {
        const value = object(
            'SBNK',
            index === 0 ? 'HH 136 Ef1 A2' : `Sample ${index} with a deliberately long object name`,
        );
        return { id: value.key, objectId: value.key, name: value.name, objectType: 'SBNK', object: value };
    });
    const sample = samples[0];
    const waveObject = object('SMPL', 'HH tekno 136B');
    const wave: WaveDataItem = {
        id: waveObject.key,
        objectKey: waveObject.key,
        object: waveObject,
        name: waveObject.name,
        note: 'C3',
        duration: '1.00 s',
        sampleRate: '44.1 kHz',
        bitDepth: '16-bit',
        channels: 'Mono',
        storedSizeBytes: 88200,
        previewState: 'ready',
        waveform: Array.from({ length: 128 }, (_, index) => ({
            minimum: -Math.abs(Math.sin(index * 1.7)) * 12000,
            maximum: Math.abs(Math.sin(index * 2.1)) * 18000,
        })),
    };
    const objects = [programObject, bankObject, ...samples.map((entry) => entry.object), waveObject];
    const edges: SamplerRelationship[] = [];
    function edge(source: SamplerObject, target: SamplerObject, type: string, channel = ''): void {
        edges.push({
            id: `edge-${edges.length}`,
            sourceObjectId: source.key,
            targetObjectId: target.key,
            candidateObjectIds: [],
            relationshipType: type,
            quality: 'KNOWN',
            basis: 'test',
            notes: [],
            assignmentName: target.name,
            assignmentState: '',
            receiveChannelDisplay: channel,
        });
    }
    for (let index = 1; index <= 16; index++) {
        const channel = `A${String(index).padStart(2, '0')}`;
        edge(programObject, sample.object, 'PROG_ASSIGNMENT_TO_SBNK', channel);
        edge(programObject, bankObject, 'PROG_ASSIGNMENT_TO_SBAC', channel);
    }
    for (const item of samples) {
        edge(programObject, item.object, 'PROG_ASSIGNMENT_TO_SBNK');
        edge(bankObject, item.object, 'SBAC_SLOT_TO_SBNK');
        edge(item.object, waveObject, 'SBNK_LEFT_MEMBER_TO_SMPL');
    }
    edge(sample.object, waveObject, 'SBNK_RIGHT_MEMBER_TO_SMPL');
    const selected =
        kind === 'program'
            ? programObject
            : kind === 'sample-bank'
              ? bankObject
              : kind === 'wave-data'
                ? waveObject
                : sample.object;
    const relationships = inspectorRelationshipGroups(
        selected.key,
        edges,
        objects.map((value) => ({
            objectId: value.key,
            objectType: value.objectType as 'PROG' | 'SBAC' | 'SBNK' | 'SMPL',
            name: value.name,
        })),
    );
    relationships[0].items.push({
        id: 'missing',
        name: 'Unresolved object with a long name',
        detail: '',
        navigable: false,
    });
    const preview = {
        item: sample,
        waveData: [{ role: 'left' as const, waveData: wave }],
        preview: null,
        previewState: 'ready' as const,
    };
    if (kind === 'program')
        return {
            kind,
            program: {
                id: selected.key,
                objectId: selected.key,
                name: selected.name,
                slot: '001',
                programNumber: 1,
                object: selected,
            },
            assignments: [],
            sampleSelect: { assigned: [], all: [] },
            relationships,
        };
    if (kind === 'sample-bank')
        return {
            kind,
            item: {
                id: selected.key,
                objectId: selected.key,
                name: selected.name,
                objectType: 'SBAC',
                object: selected,
            },
            members: samples,
            memberPreviews: [preview],
            displayedMemberId: sample.objectId,
            relationships,
        };
    if (kind === 'wave-data') return { kind, waveData: wave, relationships };
    return { kind: 'sample', item: sample, memberships: [], preview, relationships };
}
