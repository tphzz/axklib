import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import MidiExportDialog from './MidiExportDialog.svelte';

describe('MidiExportDialog', () => {
    it('uses the standard export destinations for selected Sequences', async () => {
        const onworkspace = vi.fn();
        const onlocal = vi.fn();
        render(MidiExportDialog, {
            props: {
                items: [
                    {
                        kind: 'SEQU',
                        objectId: 'sequence-1',
                        name: 'DJ TSUYOSHI DEMO',
                        typeLabel: 'Sequence',
                        partitionIndex: 0,
                        partitionName: 'Partition 0',
                        volumeName: 'DEMO',
                    },
                ],
                desktop: true,
                error: '',
                onworkspace,
                onlocal,
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('Export “DJ TSUYOSHI DEMO”')).toBeTruthy();
        expect(screen.getByText(/standard MIDI file/i)).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));
        expect(onworkspace).toHaveBeenCalledOnce();
        expect(onlocal).toHaveBeenCalledOnce();
    });

    it('summarizes a multi-Sequence export and permits closing the destination chooser', async () => {
        const oncancel = vi.fn();
        render(MidiExportDialog, {
            props: {
                items: [
                    {
                        kind: 'SEQU',
                        objectId: 'sequence-1',
                        name: 'First',
                        typeLabel: 'Sequence',
                        partitionIndex: 0,
                        partitionName: 'Partition 0',
                        volumeName: 'DEMO',
                    },
                    {
                        kind: 'SEQU',
                        objectId: 'sequence-2',
                        name: 'Second',
                        typeLabel: 'Sequence',
                        partitionIndex: 0,
                        partitionName: 'Partition 0',
                        volumeName: 'DEMO',
                    },
                ],
                desktop: false,
                error: '',
                onworkspace: vi.fn(),
                onlocal: vi.fn(),
                oncancel,
            },
        });

        expect(screen.getByText('Export 2 Sequences')).toBeTruthy();
        expect(screen.queryByRole('button', { name: /This computer/ })).toBeNull();
        await fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
        expect(oncancel).toHaveBeenCalledOnce();
    });
});
