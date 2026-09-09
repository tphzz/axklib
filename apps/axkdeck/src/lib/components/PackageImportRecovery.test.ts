import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { ImportCompletion } from '../../features/import/importCompletion.svelte';
import { JobController } from '../../features/jobs/actions';
import { serverFileLocation } from '../storageLocations';
import type { JobState, PackageInspection } from '../transport';
import PackageBatchImportDialog from './PackageBatchImportDialog.svelte';
import PackageImportDialog from './PackageImportDialog.svelte';

const job: JobState = { jobId: 7, kind: 'package-import', status: 'completed' };
const inspection: PackageInspection = {
    schemaVersion: '1.0',
    packageId: 'one',
    packageKind: 'VOLUME',
    requiredExtension: '.axkvol',
    sourceMediaKind: 'SFS',
    valid: true,
    payloadsVerified: true,
    totalPayloadBytes: 0,
    roots: [],
    objects: [],
    relationships: [],
    relationshipCount: 0,
    issues: [],
};

function showDialog(batch: boolean, completion: ImportCompletion) {
    const callbacks = { oncancel: vi.fn(), onconfirm: vi.fn(), onrecover: vi.fn() };
    const common = {
        ...callbacks,
        completion,
        desktop: false,
        plan: null,
        destinationMode: 'existing' as const,
        destinationPartitionIndex: 0,
        destinationVolumeName: 'One',
        partitionOptions: [{ partitionIndex: 0, name: 'Partition 1' }],
        volumeOptions: [],
        renames: {},
        programSlots: {},
        opaqueSequenceActions: {},
        hasUnvalidatedChanges: false,
        status: 'applying' as const,
        progress: 0,
        error: '',
        onchooseworkspace: vi.fn(),
        onchooselocal: vi.fn(),
        ondestinationmode: vi.fn(),
        ondestinationvolume: vi.fn(),
        ondestinationpartition: vi.fn(),
        ondestinationname: vi.fn(),
        onrename: vi.fn(),
        onprogramslot: vi.fn(),
        onprogramstart: vi.fn(),
        onopaquesequenceaction: vi.fn(),
        onreplan: vi.fn().mockResolvedValue(undefined),
    };
    const rendered = batch
        ? render(PackageBatchImportDialog, {
              ...common,
              canChangeSources: true,
              items: [
                  {
                      id: 'one',
                      selected: true,
                      source: serverFileLocation({ rootId: 'workspace', relativePath: 'One.axkvol' }, 'One.axkvol'),
                      sourceName: 'One.axkvol',
                      inspection,
                      upload: null,
                      localPath: null,
                  },
              ],
              destinationStrategy: 'shared',
              separateVolumesAvailable: false,
              volumeNames: {},
              completedFiles: 1,
              totalFiles: 1,
              ondestinationstrategy: vi.fn(),
              onrenamevolume: vi.fn(),
              ontoggleselected: vi.fn(),
              ontoggleall: vi.fn(),
          })
        : render(PackageImportDialog, {
              ...common,
              targetName: 'One',
              canChangeSource: true,
              sourceName: 'One.axkvol',
              inspection,
              onchange: vi.fn(),
          });
    const footer = within(rendered.container.querySelector('footer')!);
    return { ...callbacks, ...rendered, footer };
}

async function completionWith(phase: 'refresh-failed' | 'unconfirmed' | 'no-job') {
    const transport = {
        waitForJob: vi.fn().mockResolvedValue(job),
        cancelJob: vi.fn(),
    };
    const completion = new ImportCompletion(transport, new JobController(transport));
    if (phase === 'unconfirmed') transport.waitForJob.mockRejectedValueOnce(new Error('Lost result'));
    await completion.run(
        async () => {
            if (phase === 'no-job') throw new Error('Lost acknowledgement');
            return job;
        },
        async () => {
            if (phase === 'refresh-failed') throw new Error('Refresh disconnected');
        },
    );
    return { completion, transport };
}

describe.each([false, true])('package recovery dialog (batch=%s)', (batch) => {
    it('offers Refresh instead of Import, and permits identical footer/header/Escape dismissal after refresh failure', async () => {
        const { completion } = await completionWith('refresh-failed');
        const { footer, oncancel, onconfirm, onrecover } = showDialog(batch, completion);
        expect(footer.queryByRole('button', { name: 'Import' })).toBeNull();
        expect(footer.queryByRole('button', { name: 'Review' })).toBeNull();
        const closeButtons = screen.getAllByRole('button', { name: 'Close' });
        expect(closeButtons).toHaveLength(2);
        for (const button of closeButtons) {
            expect((button as HTMLButtonElement).disabled).toBe(false);
            await fireEvent.click(button);
        }
        await fireEvent.keyDown(screen.getByRole('dialog'), { key: 'Escape' });
        expect(oncancel).toHaveBeenCalledTimes(3);
        await fireEvent.click(footer.getByRole('button', { name: 'Refresh' }));
        expect(onrecover).toHaveBeenCalledOnce();
        expect(onconfirm).not.toHaveBeenCalled();
        for (const name of ['Existing', 'New', batch ? 'Change files' : 'Change']) {
            expect((screen.getByRole('button', { name }) as HTMLButtonElement).disabled).toBe(true);
        }
    });

    it('locks footer/header/Escape dismissal and only checks the acknowledged job', async () => {
        const { completion } = await completionWith('unconfirmed');
        const { footer, oncancel, onconfirm, onrecover } = showDialog(batch, completion);
        expect((footer.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: 'Close' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.keyDown(screen.getByRole('dialog'), { key: 'Escape' });
        expect(oncancel).not.toHaveBeenCalled();
        expect(footer.queryByRole('button', { name: 'Import' })).toBeNull();
        const check = footer.getByRole('button', { name: 'Check status' });
        expect((check as HTMLButtonElement).disabled).toBe(false);
        await fireEvent.click(check);
        expect(onrecover).toHaveBeenCalledOnce();
        expect(onconfirm).not.toHaveBeenCalled();
    });

    it('does not offer a status request without an acknowledged job reference', async () => {
        const { completion } = await completionWith('no-job');
        const { footer, onrecover } = showDialog(batch, completion);
        expect((footer.getByRole('button', { name: 'Check status' }) as HTMLButtonElement).disabled).toBe(true);
        expect(screen.getByRole('status').textContent).toContain('no job reference was received');
        expect(onrecover).not.toHaveBeenCalled();
    });

    it('keeps Check status in place and disabled while checking', async () => {
        const { completion, transport } = await completionWith('unconfirmed');
        let finish!: (value: JobState) => void;
        transport.waitForJob.mockImplementationOnce(
            () =>
                new Promise((resolve) => {
                    finish = resolve;
                }),
        );
        const { footer } = showDialog(batch, completion);
        const recovering = completion.recover();
        await vi.waitFor(() =>
            expect((footer.getByRole('button', { name: 'Check status' }) as HTMLButtonElement).disabled).toBe(true),
        );
        expect(footer.queryByRole('button', { name: 'Import' })).toBeNull();
        finish(job);
        await recovering;
    });

    it('keeps Refresh in place and locks dismissal while refreshing', async () => {
        const transport = { waitForJob: vi.fn().mockResolvedValue(job), cancelJob: vi.fn() };
        const completion = new ImportCompletion(transport, new JobController(transport));
        let finish!: () => void;
        const refresh = vi
            .fn()
            .mockRejectedValueOnce(new Error('Refresh disconnected'))
            .mockImplementationOnce(
                () =>
                    new Promise<void>((resolve) => {
                        finish = resolve;
                    }),
            );
        await completion.run(async () => job, refresh);
        const { footer, oncancel } = showDialog(batch, completion);
        const recovering = completion.recover();
        await vi.waitFor(() =>
            expect((footer.getByRole('button', { name: 'Refresh' }) as HTMLButtonElement).disabled).toBe(true),
        );
        expect((screen.getByRole('button', { name: 'Close' }) as HTMLButtonElement).disabled).toBe(true);
        await fireEvent.keyDown(screen.getByRole('dialog'), { key: 'Escape' });
        expect(oncancel).not.toHaveBeenCalled();
        finish();
        await recovering;
    });
});
