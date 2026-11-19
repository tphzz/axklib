import { describe, expect, it, vi } from 'vitest';
import { waveDiskRoutes, WaveDiskWorkflow } from './waveDiskWorkflow.svelte';
import {
    waveSource,
    waveProfile,
    waveRows,
    waveDownload,
    waveFixturePlan,
    waveFixtureTransport,
} from '../../test/waveDiskFixture';
import type { ConversionInput, ConversionPlan } from '../../lib/conversions';
import { serverFileLocation } from '../../lib/storageLocations';

function setup() {
    const api = waveFixtureTransport();
    const publish = vi.fn().mockResolvedValue(true),
        status = vi.fn();
    const workflow = new WaveDiskWorkflow(api, publish, status);
    const open = (ids: string[] = []) =>
        workflow.open(waveSource, 'Keys.hds', waveProfile.sourceFormat, [waveProfile], ids);
    const ready = async () => {
        await open(['wave-2', 'wave-1']);
        workflow.change({ delivery: { kind: 'download', filename: 'Studio.img' } });
        await vi.waitFor(() => expect(workflow.canCreate).toBe(true));
    };
    return { api, publish, status, workflow, open, ready };
}
describe('Wave disk conversion', () => {
    it('uses capabilities rather than a profile ID; the image-set study is unavailable', () => {
        expect(
            waveDiskRoutes(
                [
                    waveProfile,
                    { ...waveProfile, id: 'study', outputKind: 'NEW_IMAGE_SET' },
                    {
                        ...waveProfile,
                        id: 'unknown',
                        route: { ...waveProfile.route!, editorCapabilities: ['unavailable'] },
                    },
                ],
                waveProfile.sourceFormat,
            ).map((r) => r.profile.id),
        ).toEqual([waveProfile.id]);
    });
    it('inspects every page against the same source revision and disambiguates equal names by occurrence', async () => {
        const { api, workflow, open } = setup();
        const inspect = vi.spyOn(api, 'inspectConversion');
        const page = {
            sourceFormat: waveProfile.sourceFormat,
            documentId: waveSource.documentId,
            profileIds: [waveProfile.id],
            total: 2,
        };
        inspect
            .mockResolvedValueOnce({ ...page, offset: 0, items: [waveRows[0]] })
            .mockResolvedValueOnce({ ...page, offset: 1, items: [{ ...waveRows[0], id: 'same-name' }] });
        await open(['same-name', 'removed', 'wave-0']);
        expect(inspect.mock.calls).toMatchObject([
            [{ source: waveSource, offset: 0, objectType: 'SMPL' }, expect.any(AbortSignal)],
            [{ source: waveSource, offset: 1, objectType: 'SMPL' }, expect.any(AbortSignal)],
        ]);
        expect(workflow.request?.selected).toEqual(['same-name', 'wave-0']);
        expect(workflow.request?.rows).toHaveLength(2);
    });
    it('cannot prepare without an applicable inspection result', async () => {
        const { api, workflow, open } = setup();
        vi.spyOn(api, 'inspectConversion').mockResolvedValue({ ...(await api.inspectConversion()), profileIds: [] });
        const prepare = vi.spyOn(api, 'prepareConversion');
        await open(['wave-1']);
        expect(workflow.request?.error).toMatch(/No compatible/);
        expect(prepare).not.toHaveBeenCalled();
    });
    it('does not choose an arbitrary equal-priority profile when inspection permits both', async () => {
        const { api, workflow } = setup();
        const second = { ...waveProfile, id: 'alternative' };
        vi.spyOn(api, 'inspectConversion').mockResolvedValue({
            ...(await api.inspectConversion()),
            profileIds: [second.id, waveProfile.id],
        });
        await workflow.open(waveSource, 'Keys.hds', waveProfile.sourceFormat, [second, waveProfile], ['wave-0']);
        expect(workflow.request?.profile).toBeUndefined();
        expect(workflow.request?.error).toMatch(/same priority/);
        expect(workflow.canCreate).toBe(false);
    });
    it('restricts a volume by its occurrence IDs, not by its display name', async () => {
        const { api, workflow } = setup();
        const objects = [
            { ...waveRows[0], id: 'first' },
            { ...waveRows[0], id: 'second' },
        ];
        vi.spyOn(api, 'inspectConversion').mockResolvedValue({
            ...(await api.inspectConversion()),
            total: 2,
            items: objects,
        });
        await workflow.open(waveSource, 'Keys.hds', waveProfile.sourceFormat, [waveProfile], ['first', 'second'], {
            kind: 'volume',
            label: 'Partition 2 / Studio',
            locations: async () => new Map([['second', 'P2 / Studio / Bass']]),
        });
        expect(workflow.request).toMatchObject({
            selected: ['second'],
            sourceName: 'Keys.hds · Partition 2 / Studio',
            rows: [{ id: 'second', location: 'P2 / Studio / Bass' }],
            scopeKind: 'volume',
        });
    });
    it('sends ordered public IDs and the checked path; switching delivery releases the old plan', async () => {
        const { api, workflow, open } = setup();
        const prepare = vi.spyOn(api, 'prepareConversion'),
            release = vi.spyOn(api, 'releaseConversion');
        await open(['wave-2', 'wave-1']);
        const file = serverFileLocation({ rootId: 'out', relativePath: 'Studio.img' });
        workflow.change({ delivery: { kind: 'server', file } });
        await vi.waitFor(() => expect(workflow.canCreate).toBe(true));
        expect(prepare).toHaveBeenCalledWith(
            expect.objectContaining({
                source: waveSource,
                destination: { kind: 'NEW_IMAGE', file: file.reference },
                options: { documentId: 'a-series', waveIds: ['wave-2', 'wave-1'] },
            }),
            expect.any(AbortSignal),
        );
        workflow.change({ delivery: { kind: 'download', filename: 'Studio.img' }, fidelity: 'STRICT' });
        await vi.waitFor(() => expect(workflow.request?.phase).toBe('ready'));
        expect(release).toHaveBeenCalledWith('wave-plan');
        expect(workflow.canCreate).toBe(false);
    });
    it('discards a superseded plan, including one returned after Cancel', async () => {
        const { api, workflow, open } = setup();
        let complete: ((p: ConversionPlan) => void) | undefined, input: ConversionInput | undefined;
        vi.spyOn(api, 'prepareConversion').mockImplementation(async (p) => {
            input = p;
            return new Promise((resolve) => (complete = resolve));
        });
        const release = vi.spyOn(api, 'releaseConversion');
        await open(['wave-0']);
        workflow.change({ delivery: { kind: 'download', filename: 'Test.img' } });
        workflow.close();
        complete!(waveFixturePlan(input!));
        await vi.waitFor(() => expect(release).toHaveBeenCalledWith('wave-plan'));
        expect(workflow.request).toBeNull();
    });
    it('preserves the accepted plan across a lost reply and saves the created download only once', async () => {
        const { api, workflow, ready, publish } = setup();
        const run = vi.spyOn(api, 'executeConversion').mockRejectedValueOnce(new Error('response lost'));
        const recover = vi.spyOn(api, 'conversionExecution'),
            release = vi.spyOn(api, 'deleteRetainedPackage');
        await ready();
        await workflow.execute();
        expect(workflow.request?.phase).toBe('unconfirmed');
        publish.mockResolvedValueOnce(false);
        await workflow.execute();
        expect(workflow.request?.phase).toBe('save-again');
        expect(release).not.toHaveBeenCalled();
        await workflow.execute();
        expect(run).toHaveBeenCalledTimes(1);
        expect(recover).toHaveBeenCalledTimes(1);
        expect(publish).toHaveBeenLastCalledWith(waveDownload, 'Studio.img');
        expect(release).toHaveBeenCalledWith(waveDownload);
        expect(workflow.request).toBeNull();
    });
    it.each([false, true])(
        'completes a server-file publication exactly once (durability warning: %s)',
        async (warn) => {
            const { api, workflow, open, publish, status } = setup();
            const file = { rootId: 'root', relativePath: 'Studio.img' };
            vi.spyOn(api, 'waitForJob').mockResolvedValue({
                jobId: 1,
                status: 'completed',
                kind: 'conversion',
                result: {
                    planId: 'wave-plan',
                    file,
                    ...(warn
                        ? {
                              warnings: [
                                  {
                                      code: 'publication_durability_unconfirmed',
                                      message: 'Could not confirm storage durability.',
                                      context: {},
                                  },
                              ],
                          }
                        : {}),
                },
            });
            await open(['wave-0']);
            workflow.change({ delivery: { kind: 'server', file: serverFileLocation(file) } });
            await vi.waitFor(() => expect(workflow.canCreate).toBe(true));
            await workflow.execute();
            expect(publish).not.toHaveBeenCalled();
            expect(status).toHaveBeenCalledWith(
                warn ? 'Created TX16W Wave disk. Could not confirm storage durability.' : 'Created TX16W Wave disk',
            );
            expect(workflow.request).toBeNull();
        },
    );
});
