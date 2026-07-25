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
                    { kind: 'SBAC', objectId: 'bank', name: 'Drums', typeLabel: 'Sample Bank' },
                    { kind: 'SBNK', objectId: 'sample', name: 'Kick', typeLabel: 'Sample' },
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
    });
});
