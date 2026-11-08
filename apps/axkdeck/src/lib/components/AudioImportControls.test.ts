import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { createAudioImportLayoutFixture } from '../../test/audioImportLayoutFixture';
import AudioImportControls from './AudioImportControls.svelte';

describe('AudioImportControls', () => {
    it('preserves the chosen load target when files are added or removed', async () => {
        const fixture = createAudioImportLayoutFixture('normal');
        const props = {
            mode: 'SAMPLES' as const,
            sampleBankName: '',
            sampleFormat: 'A3000_188' as const,
            error: '',
            disabled: false,
            hasFiles: false,
            capacity: fixture.completion.capacity,
            destinationMode: 'existing' as const,
            partitionIndex: 0,
            volumeName: 'Target',
            partitions: [{ partitionIndex: 0, name: 'Partition' }],
            volumes: [{ partitionIndex: 0, name: 'Partition', volumeName: 'Target', label: 'Target' }],
            onmode: vi.fn(),
            onvolume: vi.fn(),
            onpartition: vi.fn(),
            onname: vi.fn(),
        };
        const view = render(AudioImportControls, { props });
        const target = screen.getByRole('group', { name: 'Sampler load target' });
        await fireEvent.click(within(target).getByRole('button', { name: 'A4K/A5K' }));
        expect(fixture.completion.capacity.target).toBe('A4000_A5000');
        await view.rerender({ ...props, hasFiles: true });
        expect(screen.getByRole('group', { name: 'Sampler load target' })).toBe(target);
        expect(fixture.completion.capacity.target).toBe('A4000_A5000');
        await view.rerender({ ...props, hasFiles: false });
        expect(screen.getByRole('group', { name: 'Sampler load target' })).toBe(target);
        expect(fixture.completion.capacity.target).toBe('A4000_A5000');
    });
});
