import { describe, expect, it, vi } from 'vitest';
import { bindFilesystemExportActions } from './filesExportActions';
import { JobController } from '../../jobs/actions';
import { PickerController } from '../../dialogs/picker';
import { serverDirectoryLocation } from '../../../lib/storageLocations';
import {
    selectLocalDirectoryExportDestination,
    saveRetainedDirectoryExport,
    cancelRetainedDirectoryExport,
} from '../../../lib/nativeDirectoryExports';

vi.mock('../../../lib/nativeDirectoryExports', () => ({
    selectLocalDirectoryExportDestination: vi.fn(),
    saveRetainedDirectoryExport: vi.fn(),
    cancelRetainedDirectoryExport: vi.fn(),
}));

function setup(local = true, desktop = true) {
    const transport = {
        connectionMode: local ? ('local' as const) : ('remote' as const),
        inspectFilesystemExport: vi.fn(),
        startFilesystemExport: vi.fn(),
        jobStatus: vi.fn(),
        cancelJob: vi.fn(),
        waitForJob: vi.fn(),
        deleteRetainedPackage: vi.fn().mockResolvedValue(undefined),
    };
    const picker = new PickerController(() => undefined);
    const choose = vi
        .spyOn(picker, 'chooseLocation')
        .mockResolvedValue(serverDirectoryLocation({ rootId: 'exports', relativePath: 'Files' }));
    const driver = bindFilesystemExportActions(
        { transport, jobs: new JobController(transport), picker, isDesktop: desktop, sessionId: () => 3 },
        3,
    );
    return { driver, transport, choose };
}

describe('Files export destinations', () => {
    it('uses one native directory picker for a local batch and never falls back to the generic chooser', async () => {
        const { driver, choose } = setup();
        vi.mocked(selectLocalDirectoryExportDestination).mockResolvedValueOnce({
            candidateId: 'candidate',
            directoryName: 'Files',
        });
        const target = await driver.chooseDestination('computer', 'Files');
        expect(target?.destination).toEqual({ kind: 'DOWNLOAD', directoryName: 'Files' });
        expect(selectLocalDirectoryExportDestination).toHaveBeenLastCalledWith('Files', 'Files');
        await expect(driver.chooseDestination('workspace', 'Files')).rejects.toThrow('direct-computer');
        expect(choose).not.toHaveBeenCalled();
        vi.mocked(selectLocalDirectoryExportDestination).mockRejectedValueOnce(new Error('Picker failed'));
        await expect(driver.chooseDestination('computer', 'Files')).rejects.toThrow('Picker failed');
        expect(choose).not.toHaveBeenCalled();
    });

    it('uses the shared remote save-directory picker and rejects native choice outside desktop', async () => {
        const { driver, choose } = setup(false, false);
        const target = await driver.chooseDestination('workspace', 'Files');
        expect(target?.destination).toEqual({
            kind: 'WORKSPACE',
            output: { rootId: 'exports', relativePath: 'Files' },
        });
        expect(choose).toHaveBeenCalledWith(
            'save-directory',
            'Export files',
            [],
            'Files',
            expect.objectContaining({ parentDialog: 'filesystem-export', requireWritableDirectory: true }),
        );
        await expect(driver.chooseDestination('computer', 'Files')).rejects.toThrow('desktop');
    });

    it('publishes and deletes only the owner-scoped retained download', async () => {
        const { driver, transport } = setup();
        vi.mocked(selectLocalDirectoryExportDestination).mockResolvedValueOnce({
            candidateId: 'candidate',
            directoryName: 'Files',
        });
        const target = await driver.chooseDestination('computer', 'Files');
        const result = {
            rootDirectory: null,
            imageId: 'image',
            revision: 7,
            entries: [],
            notices: [],
            totalBytes: 0,
            destination: 'DOWNLOAD' as const,
            output: null,
            download: {
                archiveId: 'id',
                filename: 'Files.tar',
                sizeBytes: 1024,
                expiresInSeconds: 300,
                contentPath: '/downloads/id/content',
            },
        };
        await target!.publish(result);
        expect(saveRetainedDirectoryExport).toHaveBeenCalledWith('candidate', '/downloads/id/content', 1024);
        await target!.cancelPublication!();
        expect(cancelRetainedDirectoryExport).toHaveBeenCalledWith('candidate');
        await driver.release(result);
        expect(transport.deleteRetainedPackage).toHaveBeenCalledWith(result.download);
    });
});
