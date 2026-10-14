import { describe, expect, it, vi } from 'vitest';
import { HttpConversions } from './httpConversions';
import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpJobController } from './httpJobController';
import type { HttpImageSessions } from './httpImageSessions';
import type { ConversionPlan, ConversionInput } from './conversions';
import { serverFileLocation, clientUploadLocation } from './storageLocations';
import { fitsCapacity } from '../test/samplerCapacityFixture';

function plan(offset = 0): ConversionPlan {
    return {
        planId: 'plan-1',
        state: 'READY',
        jobId: null,
        expiresAt: 1234,
        report: {
            profileId: 'yamaha.tx16w-to-a-series',
            fidelity: 'BEST',
            executable: true,
            actionCount: 201,
            issueCount: 0,
            offset,
            capacity: fitsCapacity,
            destination: { imageId: 'target', documentId: 'target-doc', expectedRevision: 9 },
            actions: [
                {
                    kind: 'wave-data',
                    name: `Wave ${offset}`,
                    sourceObject: `wave-${offset}`,
                    action: 'create',
                    details: [],
                },
            ],
            issues: [],
        },
    };
}
function fixture() {
    const client = {
        invoke: vi.fn().mockResolvedValue({ jobId: 'prepare' }),
        request: vi.fn(),
        deviceWrites: { begin: vi.fn().mockReturnValue('tracked'), acknowledge: vi.fn(), reconcile: vi.fn() },
    };
    const jobs = {
        isJob: vi.fn().mockReturnValue(true),
        map: vi.fn((job) => job),
        wait: vi.fn().mockResolvedValue({ status: 'completed', result: plan() }),
    };
    const sessions = {
        get: vi.fn((id: number) => ({
            remoteId: id === 1 ? 'source' : 'target',
            revision: 9,
            documentId: 'target-doc',
            summary: { documents: [{ id: 'target-doc' }] },
        })),
    };
    return {
        client,
        jobs,
        transport: new HttpConversions(
            client as unknown as AxklibHttpApiClient,
            jobs as unknown as HttpJobController,
            sessions as unknown as HttpImageSessions,
        ),
    };
}
const input: ConversionInput = {
    profileId: 'yamaha.tx16w-to-a-series',
    source: { kind: 'DOCUMENT', sessionId: 1, documentId: 'tx16w', expectedRevision: 4 },
    destinationSessionId: 2,
    selection: { partitionIndex: 0, volumeName: 'Samples' },
    options: { mode: 'HIERARCHY' },
    fidelity: 'BEST',
    capacityPolicy: { target: 'A4000_A5000' },
};

describe('retained conversion transport', () => {
    it('binds both document identities and revisions and retrieves every report page', async () => {
        const { client, transport } = fixture();
        client.request.mockResolvedValue(plan(200));
        const result = await transport.prepareConversion(input);
        expect(client.invoke).toHaveBeenCalledWith(
            'conversions.prepare',
            expect.objectContaining({
                source: { kind: 'DOCUMENT', imageId: 'source', documentId: 'tx16w', expectedRevision: 4 },
                destination: {
                    imageId: 'target',
                    documentId: 'target-doc',
                    expectedRevision: 9,
                    selection: input.selection,
                },
            }),
        );
        expect(client.request).toHaveBeenCalledWith('GET', '/conversion-plans/plan-1?offset=200&limit=200');
        expect(result.report?.actions.map((action) => action.name)).toEqual(['Wave 0', 'Wave 200']);
    });
    it('uses explicit file/upload references, not obsolete importer bindings', async () => {
        const { client, transport } = fixture();
        client.request.mockResolvedValue(plan(200));
        await transport.prepareConversion({
            ...input,
            source: {
                kind: 'SOURCES',
                sources: [
                    serverFileLocation({ rootId: 'root', relativePath: 'disk.ima' }),
                    clientUploadLocation({ uploadId: 'upload' }, 'DISK_IMAGE', 'disk2.ima'),
                ],
            },
        });
        expect(client.invoke.mock.calls[0][1].source).toEqual({
            kind: 'SOURCES',
            sources: [
                { kind: 'FILE', file: { rootId: 'root', relativePath: 'disk.ima' } },
                { kind: 'UPLOAD', upload: { uploadId: 'upload' } },
            ],
        });
    });
    it('recovers a lost submission through the retained job without resubmitting', async () => {
        const { client, transport } = fixture();
        client.request
            .mockRejectedValueOnce(new TypeError('Connection lost'))
            .mockResolvedValueOnce({ ...plan(), state: 'ACCEPTED', jobId: 'execution' })
            .mockResolvedValueOnce({ jobId: 'execution', state: 'RUNNING' });
        expect(await transport.executeConversion(plan(), ['issue-1'])).toEqual({
            jobId: 'execution',
            state: 'RUNNING',
        });
        expect(client.request.mock.calls.filter(([method]) => method === 'POST')).toHaveLength(1);
        expect(client.request.mock.calls[0]).toEqual([
            'POST',
            '/conversion-plans/plan-1/executions',
            { acknowledgedIssueIds: ['issue-1'] },
            { 'Idempotency-Key': 'conversion:plan-1' },
        ]);
    });
    it('releases an abandoned plan when report pagination fails', async () => {
        const { client, transport } = fixture();
        client.request.mockRejectedValueOnce(new Error('Report unavailable')).mockResolvedValueOnce({});
        await expect(transport.prepareConversion(input)).rejects.toThrow('Report unavailable');
        expect(client.request).toHaveBeenLastCalledWith('DELETE', '/conversion-plans/plan-1');
    });
});
