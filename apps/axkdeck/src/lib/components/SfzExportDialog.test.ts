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
                    wavFileCount: 2,
                    sfzEligible: true,
                    defaultDirectoryName: 'Grand Piano',
                    issues: [],
                },
                desktop: true,
                loading: false,
                error: '',
                format: 'SFZ',
                selectionMode: 'DEPENDENCY_CLOSURE',
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
                    wavFileCount: 1,
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
                error: '',
                format: 'WAV',
                selectionMode: 'DEPENDENCY_CLOSURE',
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

    it('groups repeated unconfirmed Program assignments without duplicate Svelte keys', () => {
        const relationshipIssue = {
            code: 'unconfirmed_relationship_excluded' as const,
            message: 'Unconfirmed relationship excluded from exact export',
            fatal: false as const,
            relationshipType: 'PROG_ASSIGNMENT_TO_SBAC',
            relationshipQuality: 'UNKNOWN' as const,
            reason: 'exact export requires a Known relationship',
            sourceObjectKey: 'program-002',
            candidateObjectKeys: [],
            basis: 'assignment-stored-missing-local-target',
            assignmentState: 'STORED_ASSIGNMENT' as const,
        };

        render(SfzExportDialog, {
            props: {
                items: [sample],
                inspection: {
                    imageId: 'image-one',
                    revision: 3,
                    rootCount: 1,
                    programCount: 2,
                    sampleBankCount: 1,
                    sampleCount: 1,
                    waveDataCount: 1,
                    sfzFileCount: 1,
                    wavFileCount: 1,
                    sfzEligible: true,
                    defaultDirectoryName: 'Analog Update',
                    issues: [
                        relationshipIssue,
                        {
                            ...relationshipIssue,
                            relationshipType: 'PROG_ASSIGNMENT_TO_SBNK',
                            sourceObjectKey: 'program-005',
                        },
                    ],
                },
                desktop: true,
                loading: false,
                error: '',
                format: 'SFZ',
                selectionMode: 'DEPENDENCY_CLOSURE',
                onformatchange: vi.fn(),
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('2 unconfirmed Program assignments were excluded from SFZ export.')).toBeTruthy();
        expect(screen.getAllByText(/unconfirmed Program assignments/)).toHaveLength(1);
        expect((screen.getByRole('button', { name: 'SFZ + WAV' }) as HTMLButtonElement).disabled).toBe(false);
        expect(screen.getByRole('button', { name: /Storage location/ })).toBeTruthy();
    });

    it('reviews direct Sample WAV export without offering SFZ', () => {
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
                    sfzFileCount: 0,
                    wavFileCount: 1,
                    sfzEligible: false,
                    defaultDirectoryName: 'Grand Piano WAV',
                    issues: [],
                },
                desktop: true,
                loading: false,
                error: '',
                format: 'WAV',
                selectionMode: 'SELECTED_AUDIO_OBJECTS',
                destinationFlow: 'CHOOSER',
                onformatchange: vi.fn(),
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('dialog', { name: 'Export WAV' })).toBeTruthy();
        expect(
            screen.getByText('Export each selected Sample as one mono or interleaved stereo WAV file.'),
        ).toBeTruthy();
        expect(screen.getByText('1 WAV file will be created directly in the selected folder.')).toBeTruthy();
        expect(screen.queryByRole('button', { name: 'SFZ + WAV' })).toBeNull();
    });

    it('shows a direct desktop WAV failure without generic destination choices', () => {
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
                    sfzFileCount: 0,
                    wavFileCount: 1,
                    sfzEligible: false,
                    defaultDirectoryName: 'Grand Piano WAV',
                    issues: [
                        {
                            code: 'sample_has_invalid_wave_data_membership',
                            message: 'Selected Sample does not have one mono member or an exact stereo pair.',
                            fatal: true,
                        },
                    ],
                },
                desktop: true,
                loading: false,
                error: '',
                format: 'WAV',
                selectionMode: 'SELECTED_AUDIO_OBJECTS',
                destinationFlow: 'DIRECT_COMPUTER',
                onformatchange: vi.fn(),
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText(/does not have one mono member or an exact stereo pair/)).toBeTruthy();
        expect(screen.queryByRole('button', { name: /Storage location/ })).toBeNull();
        expect(screen.queryByRole('button', { name: /This computer/ })).toBeNull();
        expect(screen.getByText('Close', { selector: 'footer button' })).toBeTruthy();
    });
});
