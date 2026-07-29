import type { components } from './generated/axklibApiV1';
import {
    inputLocationKey,
    locationKey,
    type DirectoryLocation,
    type FileLocation,
    type InputFileLocation,
    type ServerDirectoryLocation,
    type ServerFileLocation,
} from './storageLocations';
import type { InputBinding, ObjectRenameMutation, VolumeMutation } from './transport';

type WireInputBinding = components['schemas']['InputBinding'];
type WireInputRef = components['schemas']['InputRef'];

export function randomIdempotencyKey(): string {
    return globalThis.crypto?.randomUUID?.() ?? `axkdeck-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

export function serverFile(location: FileLocation): ServerFileLocation {
    if (location.kind !== 'server-file') {
        throw new Error('HTTP transport requires a server sandbox file selection');
    }
    return location;
}

export function serverInput(location: InputFileLocation): WireInputRef {
    if (location.kind === 'client-upload') {
        return { uploadRef: location.reference };
    }
    return { fileRef: serverFile(location).reference };
}

export function serverInputBindings(inputBindings: InputBinding[]): WireInputBinding[] {
    return inputBindings.map((binding) => ({
        manifestPath: binding.logicalPath,
        input: serverInput(binding.source),
    }));
}

export function serverDirectory(location: DirectoryLocation): ServerDirectoryLocation {
    if (location.kind !== 'server-directory') {
        throw new Error('HTTP transport requires a server sandbox directory selection');
    }
    return location;
}

export function createPlanKey(
    manifest: InputFileLocation,
    output: ServerFileLocation,
    overwrite: boolean,
    inputBindings: InputBinding[],
): string {
    return JSON.stringify([
        inputLocationKey(manifest),
        locationKey(output),
        overwrite,
        inputBindings.map((binding) => [binding.logicalPath, inputLocationKey(binding.source)]),
    ]);
}

export function volumeMutationOperation(mutation: VolumeMutation): Record<string, unknown> {
    const common = {
        id: `volume-${mutation.kind}`,
        partition_index: mutation.partitionIndex,
    };
    if (mutation.kind === 'add') {
        return {
            ...common,
            type: 'insert_volume',
            volume: {
                name: mutation.volumeName,
                waveforms: [],
                samples: [],
            },
        };
    }
    if (mutation.kind === 'rename') {
        return {
            ...common,
            type: 'rename_volume',
            volume_name: mutation.volumeName,
            new_volume_name: mutation.newVolumeName,
        };
    }
    return {
        ...common,
        type: 'delete_volume',
        volume_name: mutation.volumeName,
    };
}

export function objectRenameOperation(mutation: ObjectRenameMutation): Record<string, unknown> {
    const common = {
        id: `rename-${mutation.kind}`,
        partition_index: mutation.partitionIndex,
        volume_name: mutation.volumeName,
    };
    if (mutation.kind === 'program') {
        return {
            ...common,
            type: 'rename_program',
            program_number: mutation.programNumber,
            new_program_name: mutation.newProgramName,
        };
    }
    if (mutation.kind === 'sample-bank') {
        return {
            ...common,
            type: 'rename_sbac',
            sample_bank_name: mutation.sampleBankName,
            new_sample_bank_name: mutation.newSampleBankName,
        };
    }
    if (mutation.kind === 'sample') {
        return {
            ...common,
            type: 'rename_sbnk',
            sample_name: mutation.sampleName,
            new_sample_name: mutation.newSampleName,
        };
    }
    return {
        ...common,
        type: 'rename_waveform',
        waveform_name: mutation.waveformName,
        new_waveform_name: mutation.newWaveformName,
    };
}
