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
        sessions,
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
    selection: { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Samples' },
    options: { mode: 'HIERARCHY' },
    fidelity: 'BEST',
    capacityPolicy: { target: 'A4000_A5000' },
};

describe('retained conversion transport', () => {
    it('prepares a new image without obtaining a writable session or sending a capacity policy', async () => {
        const { client, jobs, sessions, transport } = fixture();
        const prepared = plan();
        prepared.report!.destination = { kind: 'NEW_IMAGE', sizeBytes: 512, sha256: 'a'.repeat(64) };
        prepared.report!.capacity = null;
        prepared.report!.actionCount = 1;
        jobs.wait.mockResolvedValue({ status: 'completed', result: prepared });
        await transport.prepareConversion({
            profileId: 'fixture.new-image',
            source: { kind: 'SOURCES', sources: [serverFileLocation({ rootId: 'root', relativePath: 'source.img' })] },
            destination: { kind: 'NEW_IMAGE' },
            options: {},
            fidelity: 'BEST',
        });
        expect(sessions.get).not.toHaveBeenCalled();
        expect(client.invoke.mock.calls[0][1].destination).toEqual({ kind: 'NEW_IMAGE' });
        expect(client.invoke.mock.calls[0][1]).not.toHaveProperty('capacityPolicy');
    });
    it('resolves workspace companions as a cancellable read job without exposing client paths', async () => {
        const { client, jobs, transport } = fixture();
        const result = {
            members: [
                {
                    source: { kind: 'FILE', file: { rootId: 'root', relativePath: 'disk.ima' } },
                    name: 'disk.ima',
                    origin: 'SELECTED',
                },
                {
                    source: { kind: 'UPLOAD', upload: { uploadId: 'second' } },
                    name: 'disk2.ima',
                    origin: 'AUTO_ADDED',
                },
            ],
            requirements: [],
        };
        jobs.wait.mockResolvedValue({ status: 'completed', result });
        const signal = new AbortController().signal;
        expect(
            await transport.resolveCompanionSources(
                {
                    sources: [serverFileLocation({ rootId: 'root', relativePath: 'disk.ima' })],
                    candidates: [clientUploadLocation({ uploadId: 'second' }, 'DISK_IMAGE', 'disk2.ima')],
                    exclusions: [serverFileLocation({ rootId: 'root', relativePath: 'removed.ima' })],
                },
                signal,
            ),
        ).toEqual(result);
        expect(client.invoke).toHaveBeenCalledWith('images.companionSources.resolve', {
            sources: [{ kind: 'FILE', file: { rootId: 'root', relativePath: 'disk.ima' } }],
            candidates: [{ kind: 'UPLOAD', upload: { uploadId: 'second' } }],
            exclusions: [{ kind: 'FILE', file: { rootId: 'root', relativePath: 'removed.ima' } }],
        });
        expect(jobs.wait).toHaveBeenCalledWith('prepare', expect.any(Function), signal);
    });
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
    it('retrieves all selectable occurrences even when output and notes fit on one page', async () => {
        const { client, jobs, transport } = fixture();
        function sourcePage(offset: number) {
            const value = plan(offset);
            const report = value.report!;
            report.actionCount = 1;
            report.sourceCount = 201;
            report.sourceObjects = Array.from({ length: offset ? 1 : 200 }, (_, i) => ({
                id: `wave-${offset + i}`,
                name: 'Piano',
                kind: 'wave',
                location: 'disk.img',
                selectable: true,
                dependencies: [],
            }));
            if (offset) report.actions = [];
            return value;
        }
        jobs.wait.mockResolvedValue({ status: 'completed', result: sourcePage(0) });
        client.request.mockResolvedValue(sourcePage(200));
        const result = await transport.prepareConversion(input);
        expect(client.request).toHaveBeenCalledWith('GET', '/conversion-plans/plan-1?offset=200&limit=200');
        expect(result.report!.sourceObjects).toHaveLength(201);
        expect(result.report!.sourceObjects!.at(-1)?.id).toBe('wave-200');
        expect(result.report!.actions).toHaveLength(1);
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
