import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { ImageTransport } from '../transport';
import { serverDirectoryLocation } from '../storageLocations';
import CreateHardDiskImageDialog from './CreateHardDiskImageDialog.svelte';

function transport(): ImageTransport {
    return {
        hardDiskCreationProfiles: vi.fn().mockResolvedValue([
            {
                profileId: 'FLOPPY_SCALE',
                sizeBytes: 1_474_560,
                defaultPartitionCount: 1,
                partitionOptions: [{ partitionCount: 1, partitionSizeBytes: 1_471_488, unusedTailBytes: 0 }],
            },
            {
                profileId: 'HDS_2_GIB',
                sizeBytes: 2_147_483_648,
                defaultPartitionCount: 2,
                partitionOptions: [
                    { partitionCount: 2, partitionSizeBytes: 1_073_740_800, unusedTailBytes: 0 },
                    { partitionCount: 3, partitionSizeBytes: 715_826_176, unusedTailBytes: 512 },
                ],
            },
        ]),
        planHardDiskCreation: vi
            .fn()
            .mockResolvedValue({ partitionCount: 2, sizeBytes: 2_147_483_648, planToken: 'plan' }),
        startHardDiskCreation: vi.fn().mockResolvedValue({ jobId: 7, kind: 'create.hds', status: 'queued' }),
        planFloppyCreation: vi.fn().mockResolvedValue({ sizeBytes: 1_474_560, planToken: 'floppy-plan' }),
        startFloppyCreation: vi.fn().mockResolvedValue({ jobId: 8, kind: 'create.floppy', status: 'queued' }),
        waitForJob: vi.fn().mockResolvedValue({ jobId: 7, kind: 'create.hds', status: 'completed', result: {} }),
        cancelJob: vi.fn().mockResolvedValue(undefined),
    } as unknown as ImageTransport;
}

describe('CreateHardDiskImageDialog', () => {
    it('renders server-admitted profile defaults and creates an exact HDS file reference', async () => {
        const imageTransport = transport();
        const onsuccess = vi.fn();
        render(CreateHardDiskImageDialog, {
            props: {
                transport: imageTransport,
                directory: serverDirectoryLocation({ rootId: 'workspace', relativePath: 'images' }, 'Yamaha/images'),
                onsuccess,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('dialog', { name: 'Create HD/Floppy image' })).toBeTruthy();
        expect((screen.getByLabelText('Type') as HTMLSelectElement).value).toBe('HD');
        expect((await screen.findByRole('button', { name: '1.44 MB' })).getAttribute('aria-pressed')).toBe('true');
        expect(screen.getByText('1 partition · 1.40 MiB each')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: '2 GiB' }));
        expect(screen.getByRole('button', { name: '2 partitions' }).getAttribute('aria-pressed')).toBe('true');
        const onePartition = screen.getByRole('button', { name: '1 partition' });
        const twoPartitions = screen.getByRole('button', { name: '2 partitions' });
        expect((onePartition as HTMLButtonElement).disabled).toBe(true);
        expect(onePartition.compareDocumentPosition(twoPartitions) & Node.DOCUMENT_POSITION_FOLLOWING).not.toBe(0);
        await fireEvent.input(screen.getByLabelText('File name'), { target: { value: 'Sampler disk.hds' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Create' }));

        await waitFor(() =>
            expect(imageTransport.planHardDiskCreation).toHaveBeenCalledWith(
                'HDS_2_GIB',
                2,
                expect.objectContaining({
                    reference: { rootId: 'workspace', relativePath: 'images/Sampler disk.hds' },
                }),
            ),
        );
        expect(imageTransport.startHardDiskCreation).toHaveBeenCalledWith('plan');
        expect(onsuccess).toHaveBeenCalledWith({
            kind: 'server-file',
            reference: { rootId: 'workspace', relativePath: 'images/Sampler disk.hds' },
            displayName: 'Yamaha/images/Sampler disk.hds',
        });
    });

    it('creates a sampler-formatted 1.44 MB FAT12 floppy without exposing HD geometry', async () => {
        const imageTransport = transport();
        vi.mocked(imageTransport.waitForJob).mockResolvedValueOnce({
            jobId: 8,
            kind: 'create.floppy',
            status: 'completed',
            result: {},
        });
        const onsuccess = vi.fn();
        render(CreateHardDiskImageDialog, {
            props: {
                transport: imageTransport,
                directory: serverDirectoryLocation({ rootId: 'workspace', relativePath: 'images' }, 'Yamaha/images'),
                onsuccess,
                oncancel: vi.fn(),
            },
        });

        await screen.findByRole('button', { name: '1.44 MB' });
        await fireEvent.change(screen.getByLabelText('Type'), { target: { value: 'Floppy' } });
        expect(screen.queryByText('Capacity')).toBeNull();
        expect(screen.queryByText('Partitions')).toBeNull();
        expect(screen.getByText('1.44 MB · Yamaha A-series FAT12 · Full format')).toBeTruthy();
        await fireEvent.input(screen.getByLabelText('File name'), { target: { value: 'Sampler disk.hds' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Create' }));

        await waitFor(() =>
            expect(imageTransport.planFloppyCreation).toHaveBeenCalledWith(
                expect.objectContaining({
                    reference: { rootId: 'workspace', relativePath: 'images/Sampler disk.ima' },
                }),
            ),
        );
        expect(imageTransport.startFloppyCreation).toHaveBeenCalledWith('floppy-plan');
        expect(imageTransport.planHardDiskCreation).not.toHaveBeenCalled();
        expect(onsuccess).toHaveBeenCalledWith({
            kind: 'server-file',
            reference: { rootId: 'workspace', relativePath: 'images/Sampler disk.ima' },
            displayName: 'Yamaha/images/Sampler disk.ima',
        });
    });

    it('keeps invalid names and server failures in the dialog', async () => {
        const imageTransport = transport();
        vi.mocked(imageTransport.planHardDiskCreation).mockRejectedValueOnce(new Error('Destination already exists'));
        render(CreateHardDiskImageDialog, {
            props: {
                transport: imageTransport,
                directory: serverDirectoryLocation({ rootId: 'workspace', relativePath: '' }, 'Yamaha'),
                onsuccess: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        await screen.findByRole('button', { name: '1.44 MB' });
        await fireEvent.input(screen.getByLabelText('File name'), { target: { value: '../bad' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Create' }));
        expect(screen.getByRole('alert').textContent).toContain('without directory separators');

        await fireEvent.input(screen.getByLabelText('File name'), { target: { value: 'Existing' } });
        await fireEvent.click(screen.getByRole('button', { name: 'Create' }));
        expect(await screen.findByText('Destination already exists')).toBeTruthy();
    });

    it('keeps the dialog open until a cancelled job reaches its terminal state', async () => {
        const imageTransport = transport();
        let finishJob!: (job: { jobId: number; kind: string; status: 'cancelled' }) => void;
        vi.mocked(imageTransport.waitForJob).mockImplementation(
            () =>
                new Promise((resolve) => {
                    finishJob = resolve;
                }),
        );
        const oncancel = vi.fn();
        render(CreateHardDiskImageDialog, {
            props: {
                transport: imageTransport,
                directory: serverDirectoryLocation({ rootId: 'workspace', relativePath: '' }, 'Yamaha'),
                onsuccess: vi.fn(),
                oncancel,
            },
        });

        await screen.findByRole('button', { name: '1.44 MB' });
        await fireEvent.click(screen.getByRole('button', { name: 'Create' }));
        await waitFor(() => expect(imageTransport.waitForJob).toHaveBeenCalledOnce());
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));

        expect(imageTransport.cancelJob).toHaveBeenCalledWith(7);
        expect(oncancel).not.toHaveBeenCalled();
        expect(screen.getByRole('button', { name: 'Cancelling' })).toBeTruthy();

        finishJob({ jobId: 7, kind: 'create.hds', status: 'cancelled' });
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
    });
});
