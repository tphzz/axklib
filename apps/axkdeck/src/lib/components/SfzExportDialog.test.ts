import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import SfzExportDialog from './SfzExportDialog.svelte';

const sample = {
    kind: 'SBNK' as const,
    objectId: 'sample-one',
    name: 'Grand Piano',
    typeLabel: 'Sample' as const,
    partitionIndex: 0,
    partitionName: 'Partition 0',
    volumeName: 'PIANOS',
};

describe('SfzExportDialog', () => {
    it('uses the package export destination choices and reports the resolved closure', async () => {
        const onworkspace = vi.fn();
        const onlocal = vi.fn();
        render(SfzExportDialog, {
            props: {
                items: [sample],
                inspection: {
                    imageId: 'image-one',
                    revision: 3,
                    rootCount: 1,
                    programCount: 0,
                    sampleBankCount: 0,
                    sampleCount: 1,
                    waveDataCount: 2,
                    sfzFileCount: 1,
                    sfzEligible: true,
                    defaultDirectoryName: 'Grand Piano',
                    issues: [],
                },
                desktop: true,
                loading: false,
                busy: false,
                progressLabel: '',
                error: '',
                format: 'SFZ',
                onformatchange: vi.fn(),
                onworkspace,
                onlocal,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('1 Sample · 2 Wave Data')).toBeTruthy();
        expect(screen.getByText('1 SFZ file and referenced WAV files will be created.')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));
        expect(onworkspace).toHaveBeenCalledOnce();
        expect(onlocal).toHaveBeenCalledOnce();
    });

    it('selects WAV export when reliable SFZ semantics are unavailable', async () => {
        const onformatchange = vi.fn();
        render(SfzExportDialog, {
            props: {
                items: [sample],
                inspection: {
                    imageId: 'image-one',
                    revision: 3,
                    rootCount: 1,
                    programCount: 0,
                    sampleBankCount: 0,
                    sampleCount: 0,
                    waveDataCount: 1,
                    sfzFileCount: 0,
                    sfzEligible: false,
                    defaultDirectoryName: 'Orphan Wave Data',
                    issues: [
                        {
                            code: 'wave_data_has_no_confirmed_sample',
                            message: 'Selected Wave Data has no confirmed referencing Sample; export it as WAV.',
                            fatal: true,
                        },
                    ],
                },
                desktop: false,
                loading: false,
                busy: false,
                progressLabel: '',
                error: '',
                format: 'WAV',
                onformatchange,
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect((screen.getByRole('button', { name: 'SFZ + WAV' }) as HTMLButtonElement).disabled).toBe(true);
        expect(screen.getByText(/SFZ mappings are unavailable/)).toBeTruthy();
        expect(screen.getByText(/no confirmed referencing Sample/)).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: 'WAV files' }));
        expect(onformatchange).toHaveBeenCalledWith('WAV');
    });
});
