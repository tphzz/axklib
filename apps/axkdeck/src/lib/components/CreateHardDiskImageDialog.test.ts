import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { HardDiskCreationProfile, HardDiskCreationProfileId, ImageTransport } from '../transport';
import { serverDirectoryLocation } from '../storageLocations';
import CreateHardDiskImageDialog from './CreateHardDiskImageDialog.svelte';

function profile(
    profileId: HardDiskCreationProfileId,
    sizeBytes: number,
    defaultPartitionCount: number,
    maximumPartitionCount = 8,
): HardDiskCreationProfile {
    return {
        profileId,
        sizeBytes,
        defaultPartitionCount,
        partitionOptions: Array.from({ length: maximumPartitionCount - defaultPartitionCount + 1 }, (_, index) => {
            const partitionCount = defaultPartitionCount + index;
            const slotSectors = Math.min(Math.floor((sizeBytes / 512 - 2) / partitionCount), 0x1fffff);
            return {
                partitionCount,
                partitionSizeBytes: (slotSectors - 1) * 512,
                unusedTailBytes: sizeBytes - (2 + partitionCount * slotSectors) * 512,
            };
        }),
    };
}

function transport(): ImageTransport {
    return {
        hardDiskCreationProfiles: vi
            .fn()
            .mockResolvedValue([
                profile('FLOPPY_SCALE', 1_474_560, 1, 1),
                profile('HDS_128_MIB', 134_217_728, 1),
                profile('HDS_256_MIB', 268_435_456, 1),
                profile('CD_R_650', 681_984_000, 1),
                profile('CD_R_700', 737_280_000, 1),
                profile('HDS_1_GIB', 1_073_741_824, 1),
                profile('HDS_2_GIB', 2_147_483_648, 2),
                profile('HDS_4_GIB', 4_294_967_296, 4),
                profile('HDS_8_GIB', 8_589_934_592, 8),
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
    it.each([
        ['HDS_128_MIB', '128 MiB', 1],
        ['HDS_256_MIB', '256 MiB', 1],
        ['HDS_4_GIB', '4 GiB', 4],
        ['HDS_8_GIB', '8 GiB', 8],
    ] as const)(
        'selects %s with admitted counts and a reason for disabled choices',
        async (id, label, minimumCount) => {
            const imageTransport = transport();
            render(CreateHardDiskImageDialog, {
                props: {
                    transport: imageTransport,
                    directory: serverDirectoryLocation({ rootId: 'workspace', relativePath: '' }, 'Yamaha'),
                    onsuccess: vi.fn(),
                    oncancel: vi.fn(),
                },
            });

            await screen.findByRole('option', { name: label });
            const capacity = screen.getByRole('combobox', { name: 'Capacity' }) as HTMLSelectElement;
            expect(Array.from(capacity.options, (option) => option.textContent)).toEqual([
                '1.44 MB',
                '128 MiB',
                '256 MiB',
                'CD-R 650',
                'CD-R 700',
                '1 GiB',
                '2 GiB',
                '4 GiB',
                '8 GiB',
            ]);
            await fireEvent.change(capacity, { target: { value: id } });

            expect(capacity.value).toBe(id);
            for (let count = 1; count <= 8; count += 1) {
                const button = screen.getByRole('button', {
                    name: `${count} ${count === 1 ? 'partition' : 'partitions'}`,
                    ...(count < minimumCount
                        ? { description: 'This partition count is not supported for the selected capacity.' }
                        : {}),
                }) as HTMLButtonElement;
                expect(button.disabled).toBe(count < minimumCount);
                expect(button.getAttribute('aria-pressed')).toBe(String(count === minimumCount));
            }

            await fireEvent.click(screen.getByRole('button', { name: 'Create' }));
            await waitFor(() =>
                expect(imageTransport.planHardDiskCreation).toHaveBeenCalledWith(
                    id,
                    minimumCount,
                    expect.objectContaining({ reference: { rootId: 'workspace', relativePath: 'New disk.hds' } }),
                ),
            );
        },
    );

    it('preserves admitted partition counts and resets unsupported counts to the next profile default', async () => {
        render(CreateHardDiskImageDialog, {
            props: {
                transport: transport(),
                directory: serverDirectoryLocation({ rootId: 'workspace', relativePath: '' }, 'Yamaha'),
                onsuccess: vi.fn(),
                oncancel: vi.fn(),
            },
        });
        await screen.findByRole('option', { name: '128 MiB' });
        const capacity = screen.getByRole('combobox', { name: 'Capacity' });
        await fireEvent.change(capacity, { target: { value: 'HDS_128_MIB' } });
        await fireEvent.click(screen.getByRole('button', { name: '6 partitions' }));

        for (const id of ['HDS_256_MIB', 'HDS_4_GIB']) {
            await fireEvent.change(capacity, { target: { value: id } });
            expect(screen.getByRole('button', { name: '6 partitions' }).getAttribute('aria-pressed')).toBe('true');
        }
        await fireEvent.change(capacity, { target: { value: 'HDS_8_GIB' } });
        expect(screen.getByRole('button', { name: '8 partitions' }).getAttribute('aria-pressed')).toBe('true');
        await fireEvent.change(capacity, { target: { value: 'HDS_128_MIB' } });
        expect(screen.getByRole('button', { name: '8 partitions' }).getAttribute('aria-pressed')).toBe('true');
        await fireEvent.change(capacity, { target: { value: 'FLOPPY_SCALE' } });
        expect(screen.getByRole('button', { name: '1 partition' }).getAttribute('aria-pressed')).toBe('true');
        await fireEvent.change(capacity, { target: { value: 'HDS_256_MIB' } });
        await fireEvent.click(screen.getByRole('button', { name: '3 partitions' }));
        await fireEvent.change(capacity, { target: { value: 'HDS_4_GIB' } });
        expect(screen.getByRole('button', { name: '4 partitions' }).getAttribute('aria-pressed')).toBe('true');
    });

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
        await screen.findByRole('option', { name: '1.44 MB' });
        expect((screen.getByRole('combobox', { name: 'Capacity' }) as HTMLSelectElement).value).toBe('FLOPPY_SCALE');
        expect(screen.getByText('1 partition · 1.40 MiB each')).toBeTruthy();
        await fireEvent.change(screen.getByRole('combobox', { name: 'Capacity' }), { target: { value: 'HDS_2_GIB' } });
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

        await screen.findByRole('option', { name: '1.44 MB' });
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

        await screen.findByRole('option', { name: '1.44 MB' });
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

        await screen.findByRole('option', { name: '1.44 MB' });
        await fireEvent.click(screen.getByRole('button', { name: 'Create' }));
        await waitFor(() => expect(imageTransport.waitForJob).toHaveBeenCalledOnce());
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));

        expect(imageTransport.cancelJob).toHaveBeenCalledWith(7);
        expect(oncancel).not.toHaveBeenCalled();
        expect(screen.getByRole('button', { name: 'Cancel' })).toBeTruthy();
        expect(screen.getByRole('button', { name: 'Create' })).toBeTruthy();
        expect(screen.getByText('Cancelling...', { selector: '.dialog-footer-status' }).getAttribute('role')).toBe(
            'status',
        );

        finishJob({ jobId: 7, kind: 'create.hds', status: 'cancelled' });
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
    });
});
