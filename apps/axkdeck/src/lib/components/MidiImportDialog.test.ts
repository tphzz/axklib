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

describe('MidiImportDialog', () => {
    it('offers the shared workspace and local source choices before staging files', async () => {
        const onchooseworkspace = vi.fn();
        const onchooselocal = vi.fn();
        render(MidiImportDialog, {
            props: {
                transport: transport(),
                files: [],
                target: { partitionIndex: 0, volumeName: 'Songs' },
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
                target: { partitionIndex: 1, volumeName: 'Songs' },
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
                target: { partitionIndex: 1, volumeName: 'Songs' },
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
                target: { partitionIndex: 1, volumeName: 'Songs' },
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
                target: { partitionIndex: 1, volumeName: 'Songs' },
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
});
