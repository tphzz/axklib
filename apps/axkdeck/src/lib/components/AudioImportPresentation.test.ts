import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import { createAudioImportLayoutFixture } from '../../test/audioImportLayoutFixture';
import { selectValue } from '../../test/selectControl';
import AudioImportDialog from './AudioImportDialog.svelte';

function renderDialog() {
    const fixture = createAudioImportLayoutFixture('normal');
    render(AudioImportDialog, {
        props: {
            transport: fixture.transport,
            completion: fixture.completion,
            files: fixture.files,
            target: { kind: 'EXISTING_VOLUME', partitionIndex: 0, volumeName: 'Target' },
            destinationMode: 'existing',
            destinationPartitionIndex: 0,
            destinationVolumeName: 'Target',
            partitionOptions: [{ partitionIndex: 0, name: 'Partition' }],
            volumeOptions: [{ partitionIndex: 0, name: 'Partition', volumeName: 'Target', label: 'Target' }],
            existingSampleNames: ['Reserved'],
            existingWaveformNames: ['Reserved wave'],
            ondestinationmode: vi.fn(),
            ondestinationvolume: vi.fn(),
            ondestinationpartition: vi.fn(),
            ondestinationname: vi.fn(),
            oncommit: vi.fn().mockResolvedValue(false),
            oncancel: vi.fn(),
        },
    });
    return fixture;
}

function expectAssociatedError(field: HTMLElement, message: string) {
    expect(field.getAttribute('aria-invalid')).toBe('true');
    const error = screen.getByRole('alert');
    expect(error.textContent).toBe(message);
    expect(field.getAttribute('aria-describedby')).toBe(error.id);
}

describe('Audio import field feedback', () => {
    it('associates a name conflict only with the affected names and clears it after correction', async () => {
        renderDialog();
        const sample = (await screen.findAllByLabelText(/^Sample name for/))[0];
        const wave = screen.getAllByLabelText(/^Wave data \(mono\/left\) for/)[0];
        await waitFor(() => expect((sample as HTMLInputElement).value).not.toBe(''));
        await fireEvent.input(sample, { target: { value: 'Reserved' } });
        expectAssociatedError(sample, 'Sample name already exists: Reserved');
        expect(wave.getAttribute('aria-invalid')).not.toBe('true');
        await fireEvent.input(sample, { target: { value: 'Unique sample' } });
        expect(sample.hasAttribute('aria-describedby')).toBe(false);
        expect(sample.getAttribute('aria-invalid')).not.toBe('true');
        await fireEvent.input(wave, { target: { value: 'Reserved wave' } });
        expectAssociatedError(wave, 'Wave data name already exists: Reserved wave');
        expect(sample.getAttribute('aria-invalid')).not.toBe('true');
        expect(screen.getAllByLabelText(/^Wave data \(right\) for/)[0].getAttribute('aria-invalid')).not.toBe('true');
        await fireEvent.input(wave, { target: { value: '' } });
        expect(screen.queryByText('Name conflict')).toBeNull();
        expect(screen.getByText('Needs attention')).toBeTruthy();
    });

    it('associates a range error with both range bounds without marking unrelated controls', async () => {
        renderDialog();
        const sample = (await screen.findAllByLabelText(/^Sample name for/))[0];
        await waitFor(() => expect((sample as HTMLInputElement).value).not.toBe(''));
        const low = screen.getAllByLabelText(/^Lowest key for/)[0];
        const high = screen.getAllByLabelText(/^Highest key for/)[0];
        await fireEvent.input(low, { target: { value: '100' } });
        await fireEvent.input(high, { target: { value: '20' } });
        expectAssociatedError(low, 'Key range must be an ordered range from 0 to 127.');
        expectAssociatedError(high, 'Key range must be an ordered range from 0 to 127.');
        expect(screen.getAllByLabelText(/^Lowest velocity for/)[0].getAttribute('aria-invalid')).not.toBe('true');
        await fireEvent.input(high, { target: { value: '127' } });
        expect(screen.queryByRole('alert')).toBeNull();
        expect(low.hasAttribute('aria-describedby')).toBe(false);
    });

    it('excludes a stale inspection from the prepared summary while checking and after failure', async () => {
        const fixture = renderDialog();
        const rates = await screen.findAllByRole('combobox', { name: /^Target sample rate for/ });
        await screen.findByText(/6 files · 6 Samples/);
        let rejectInspection!: (error: Error) => void;
        vi.spyOn(fixture.transport, 'inspectAudio').mockImplementationOnce(
            () => new Promise((_resolve, reject) => (rejectInspection = reject)),
        );
        await selectValue(rates[0], 22050);
        expect(screen.getByText(/6 files · 5 prepared/)).toBeTruthy();
        rejectInspection(new Error('Source unavailable during reinspection'));
        await screen.findByText('Source unavailable during reinspection');
        expect(screen.getByText(/6 files · 5 prepared/)).toBeTruthy();
    });
});
