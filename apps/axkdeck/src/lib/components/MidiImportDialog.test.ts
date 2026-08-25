import { fireEvent, render, screen, waitFor } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import type { ClientUploadSource } from '../clientUploadSource';
import { clientUploadLocation, serverFileLocation } from '../storageLocations';
import type { ImageTransport, MidiInspection } from '../transport';
import MidiImportDialog from './MidiImportDialog.svelte';

function inspection(overrides: Partial<MidiInspection> = {}): MidiInspection {
    return {
        format: 0,
        trackCount: 1,
        ticksPerQuarterNote: 96,
        endTick: 192,
        eventCount: 4,
        channelEventCount: 3,
        metaEventCount: 1,
        systemExclusiveEventCount: 0,
        systemExclusiveDataBytes: 0,
        controllers: [],
        systemExclusiveManufacturerIds: [],
        systemExclusivePreservationSupported: false,
        ...overrides,
    };
}

function transport(midiInspection = inspection()): ImageTransport {
    return {
        uploadClientFile: vi.fn(async (file: ClientUploadSource, kind, onProgress) => {
            onProgress?.(file.size, file.size);
            return clientUploadLocation({ uploadId: 'midi-upload' }, kind, file.name);
        }),
        releaseClientUpload: vi.fn().mockResolvedValue(undefined),
        inspectMidi: vi.fn().mockResolvedValue(midiInspection),
    } as unknown as ImageTransport;
}

function destinationProps() {
    return {
        target: { kind: 'EXISTING_VOLUME' as const, partitionIndex: 1, volumeName: 'Songs' },
        destinationMode: 'existing' as const,
        destinationPartitionIndex: 1,
        destinationVolumeName: 'Songs',
        partitionOptions: [
            { partitionIndex: 0, name: 'PARTITION 1' },
            { partitionIndex: 1, name: 'PARTITION 2' },
        ],
        volumeOptions: [{ partitionIndex: 1, name: 'PARTITION 2', volumeName: 'Songs', label: 'PARTITION 2 / Songs' }],
        destinationBusy: false,
        ondestinationmode: vi.fn(),
        ondestinationvolume: vi.fn(),
        ondestinationpartition: vi.fn(),
        ondestinationname: vi.fn(),
    };
}

describe('MidiImportDialog', () => {
    it('offers the shared workspace and local source choices before staging files', async () => {
        const onchooseworkspace = vi.fn();
        const onchooselocal = vi.fn();
        render(MidiImportDialog, {
            props: {
                transport: transport(),
                files: [],
                ...destinationProps(),
                existingSequenceNames: [],
                onchooseworkspace,
                onchooselocal,
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect(screen.getByText('Choose MIDI files')).toBeTruthy();
        await fireEvent.click(screen.getByRole('button', { name: /Storage location/ }));
        await fireEvent.click(screen.getByRole('button', { name: /This computer/ }));
        expect(onchooseworkspace).toHaveBeenCalledOnce();
        expect(onchooselocal).toHaveBeenCalledOnce();
    });

    it('uses collision-free Sequence names and imports workspace files directly', async () => {
        const imageTransport = transport();
        const oncommit = vi.fn().mockResolvedValue(undefined);
        const workspaceFile = serverFileLocation(
            { rootId: 'workspace', relativePath: 'midi/Demo Song.mid' },
            'Yamaha/midi/Demo Song.mid',
        );
        render(MidiImportDialog, {
            props: {
                transport: imageTransport,
                files: [workspaceFile],
                ...destinationProps(),
                existingSequenceNames: ['Demo Song'],
                oncommit,
                oncancel: vi.fn(),
            },
        });

        expect(await screen.findByDisplayValue('Demo Song 2')).toBeTruthy();
        const importButton = screen.getByRole('button', { name: 'Import 1 file' });
        await waitFor(() => expect((importButton as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(importButton);
        await waitFor(() =>
            expect(oncommit).toHaveBeenCalledWith([{ source: workspaceFile, sequenceName: 'Demo Song 2' }], 'exclude'),
        );
        expect(imageTransport.uploadClientFile).not.toHaveBeenCalled();
    });

    it('uploads local MIDI with the MIDI contract and releases it after import', async () => {
        const imageTransport = transport();
        const oncommit = vi.fn().mockResolvedValue(undefined);
        const oncancel = vi.fn();
        const file = new File([new Uint8Array(32)], 'Pattern.mid', { type: 'audio/midi' });
        render(MidiImportDialog, {
            props: {
                transport: imageTransport,
                files: [file],
                ...destinationProps(),
                existingSequenceNames: [],
                oncommit,
                oncancel,
            },
        });

        expect(await screen.findByDisplayValue('Pattern')).toBeTruthy();
        expect(imageTransport.uploadClientFile).toHaveBeenCalledWith(
            expect.objectContaining({ name: 'Pattern.mid' }),
            'MIDI',
            expect.any(Function),
            expect.any(AbortSignal),
        );
        const importButton = screen.getByRole('button', { name: 'Import 1 file' });
        await waitFor(() => expect((importButton as HTMLButtonElement).disabled).toBe(false));
        await fireEvent.click(importButton);
        await waitFor(() =>
            expect(imageTransport.releaseClientUpload).toHaveBeenCalledWith(
                expect.objectContaining({ reference: { uploadId: 'midi-upload' } }),
            ),
        );
        await waitFor(() => expect(oncancel).toHaveBeenCalledOnce());
    });

    it('keeps inclusion unavailable when a SysEx event is outside the admitted preservation profile', async () => {
        const imageTransport = transport(
            inspection({
                systemExclusiveEventCount: 2,
                systemExclusiveDataBytes: 48,
                systemExclusiveManufacturerIds: ['43'],
            }),
        );
        const oncommit = vi.fn().mockResolvedValue(undefined);
        const workspaceFile = serverFileLocation(
            { rootId: 'workspace', relativePath: 'midi/System.mid' },
            'Yamaha/midi/System.mid',
        );
        render(MidiImportDialog, {
            props: {
                transport: imageTransport,
                files: [workspaceFile],
                ...destinationProps(),
                existingSequenceNames: [],
                oncommit,
                oncancel: vi.fn(),
            },
        });

        const include = await screen.findByRole('checkbox', { name: 'Include SysEx events' });
        expect((include as HTMLInputElement).checked).toBe(false);
        expect(await screen.findByText(/2 SysEx events will be excluded/i)).toBeTruthy();
        expect((include as HTMLInputElement).disabled).toBe(true);
        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() => expect(oncommit).toHaveBeenCalledWith(expect.any(Array), 'exclude'));
    });

    it('passes preserve only after the server admits SysEx preservation and the user enables it', async () => {
        const imageTransport = transport(
            inspection({
                systemExclusiveEventCount: 1,
                systemExclusiveDataBytes: 12,
                systemExclusivePreservationSupported: true,
            }),
        );
        const oncommit = vi.fn().mockResolvedValue(undefined);
        const workspaceFile = serverFileLocation(
            { rootId: 'workspace', relativePath: 'midi/System.mid' },
            'Yamaha/midi/System.mid',
        );
        render(MidiImportDialog, {
            props: {
                transport: imageTransport,
                files: [workspaceFile],
                ...destinationProps(),
                existingSequenceNames: [],
                oncommit,
                oncancel: vi.fn(),
            },
        });

        const include = await screen.findByRole('checkbox', { name: 'Include SysEx events' });
        await waitFor(() => expect((include as HTMLInputElement).disabled).toBe(false));
        await fireEvent.click(include);
        await fireEvent.click(screen.getByRole('button', { name: 'Import 1 file' }));
        await waitFor(() => expect(oncommit).toHaveBeenCalledWith(expect.any(Array), 'preserve'));
    });

    it('uses the shared destination chooser without resetting edited Sequence names', async () => {
        const workspaceFile = serverFileLocation(
            { rootId: 'workspace', relativePath: 'midi/Pattern.mid' },
            'Yamaha/midi/Pattern.mid',
        );
        const props = destinationProps();
        render(MidiImportDialog, {
            props: {
                transport: transport(),
                files: [workspaceFile],
                ...props,
                existingSequenceNames: [],
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        expect((screen.getByRole('combobox', { name: 'Destination volume' }) as HTMLInputElement).value).toBe('Songs');
        const sequenceName = await screen.findByDisplayValue('Pattern');
        await fireEvent.input(sequenceName, { target: { value: 'Edited' } });
        await fireEvent.click(screen.getByRole('button', { name: 'New' }));

        expect(props.ondestinationmode).toHaveBeenCalledWith('create');
        expect(screen.getByDisplayValue('Edited')).toBeTruthy();
    });

    it('keeps the destination autocomplete outside the scrolling MIDI content', async () => {
        const workspaceFile = serverFileLocation(
            { rootId: 'workspace', relativePath: 'midi/Pattern.mid' },
            'Yamaha/midi/Pattern.mid',
        );
        const { container } = render(MidiImportDialog, {
            props: {
                transport: transport(),
                files: [workspaceFile],
                ...destinationProps(),
                existingSequenceNames: [],
                oncommit: vi.fn(),
                oncancel: vi.fn(),
            },
        });

        const destination = container.querySelector('.import-destination');
        const scrollingContent = container.querySelector('.midi-import-body');
        const dialog = container.querySelector('.midi-import-dialog');
        expect(destination).toBeTruthy();
        expect(scrollingContent).toBeTruthy();
        expect(scrollingContent?.contains(destination)).toBe(false);
        expect(dialog?.classList.contains('dialog-popovers-visible')).toBe(true);

        await fireEvent.focus(screen.getByRole('combobox', { name: 'Destination volume' }));
        expect(screen.getByRole('listbox', { name: 'Volumes' })).toBeTruthy();
    });
});
