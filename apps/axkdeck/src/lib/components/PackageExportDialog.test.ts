import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import PackageExportDialog from './PackageExportDialog.svelte';

describe('PackageExportDialog', () => {
    it('offers configured storage and a desktop-local destination', async () => {
        const onworkspace = vi.fn();
        const onlocal = vi.fn();
        render(PackageExportDialog, {
            props: {
                items: [
                    {
                        kind: 'VOLUME',
                        partitionIndex: 0,
                        volumeName: 'DRUMS',
                        name: 'DRUMS',
                        typeLabel: 'Volume',
                    },
                ],
                desktop: true,
                busy: false,
                progressLabel: '',
                error: '',
                onworkspace,
                onlocal,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('Export “DRUMS”')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));
        expect(onworkspace).toHaveBeenCalledOnce();
        expect(onlocal).toHaveBeenCalledOnce();
    });

    it('hides local save in browser mode and disables actions while exporting', () => {
        render(PackageExportDialog, {
            props: {
                items: [
                    {
                        kind: 'SBNK',
                        objectId: 'object-piano',
                        name: 'PIANOS',
                        typeLabel: 'Sample',
                        partitionIndex: 0,
                        partitionName: 'Partition 0',
                        volumeName: 'Volume',
                    },
                ],
                desktop: false,
                busy: true,
                progressLabel: 'Writing package',
                error: '',
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.queryByRole('button', { name: /This computer/ })).toBeNull();
        expect((screen.getByRole('button', { name: /Storage location/ }) as HTMLButtonElement).disabled).toBe(true);
        expect((screen.getByRole('button', { name: 'Cancel' }) as HTMLButtonElement).disabled).toBe(true);
        expect(screen.getByRole('status').textContent).toContain('Writing package');
    });

    it('summarizes every root in a multi-object package', () => {
        render(PackageExportDialog, {
            props: {
                items: [
                    {
                        kind: 'SBAC',
                        objectId: 'bank',
                        name: 'Drums',
                        typeLabel: 'Sample Bank',
                        partitionIndex: 0,
                        partitionName: 'Partition 0',
                        volumeName: 'DRUMS',
                    },
                    {
                        kind: 'SBNK',
                        objectId: 'sample',
                        name: 'Kick',
                        typeLabel: 'Sample',
                        partitionIndex: 1,
                        partitionName: 'Partition 1',
                        volumeName: 'KITS',
                    },
                ],
                desktop: false,
                busy: false,
                progressLabel: '',
                error: '',
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('Export 2 objects')).toBeTruthy();
        expect(screen.getByLabelText('Selected objects').textContent).toContain('Drums');
        expect(screen.getByLabelText('Selected objects').textContent).toContain('Kick');
        expect(screen.getByText('Partition 0 · DRUMS')).toBeTruthy();
        expect(screen.getByText('Partition 1 · KITS')).toBeTruthy();
        expect(screen.getByText('1 Sample Bank · 1 Sample')).toBeTruthy();
    });

    it('summarizes multiple Sequences explicitly', () => {
        render(PackageExportDialog, {
            props: {
                items: [
                    {
                        kind: 'SEQU',
                        objectId: 'sequence-a',
                        name: 'Intro',
                        typeLabel: 'Sequence',
                        partitionIndex: 0,
                        partitionName: 'Partition 0',
                        volumeName: 'SONGS',
                    },
                    {
                        kind: 'SEQU',
                        objectId: 'sequence-b',
                        name: 'Finale',
                        typeLabel: 'Sequence',
                        partitionIndex: 0,
                        partitionName: 'Partition 0',
                        volumeName: 'SONGS',
                    },
                ],
                desktop: false,
                busy: false,
                progressLabel: '',
                error: '',
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('2 Sequences')).toBeTruthy();
        expect(screen.getByLabelText('Selected objects').textContent).toContain('Intro');
        expect(screen.getByLabelText('Selected objects').textContent).toContain('Finale');
    });

    it('distinguishes same-named partition and volume groups by partition index', () => {
        render(PackageExportDialog, {
            props: {
                items: [
                    {
                        kind: 'SBNK',
                        objectId: 'sample-a',
                        name: 'Kick',
                        typeLabel: 'Sample',
                        partitionIndex: 0,
                        partitionName: 'Shared',
                        volumeName: 'DRUMS',
                    },
                    {
                        kind: 'SBNK',
                        objectId: 'sample-b',
                        name: 'Snare',
                        typeLabel: 'Sample',
                        partitionIndex: 1,
                        partitionName: 'Shared',
                        volumeName: 'DRUMS',
                    },
                ],
                desktop: false,
                busy: false,
                progressLabel: '',
                error: '',
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('Shared [Partition 0] · DRUMS')).toBeTruthy();
        expect(screen.getByText('Shared [Partition 1] · DRUMS')).toBeTruthy();
    });

    it('presents unresolved Sample Bank members without internal resolver labels', () => {
        const error =
            'Sample Bank package export cannot unambiguously identify every Sample member in its source volume';
        render(PackageExportDialog, {
            props: {
                items: [
                    {
                        kind: 'SBAC',
                        objectId: 'bank',
                        name: 'Ambiguous Bank',
                        typeLabel: 'Sample Bank',
                        partitionIndex: 0,
                        partitionName: 'Partition 0',
                        volumeName: 'SOURCE',
                    },
                ],
                desktop: false,
                busy: false,
                progressLabel: '',
                error,
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByRole('alert').textContent).toBe(error);
        expect(screen.getByRole('alert').textContent).not.toMatch(/\bKnown\b|\bLikely\b|\bTentative\b/);
    });
});
