import type { components } from './generated/axklibApiV1';
import type { ImageLocation } from './storageLocations';

export function imageSourceReference(location: ImageLocation): components['schemas']['ImageSourceRef'] {
    if (location.kind === 'device') return { kind: 'DEVICE', device: location.reference };
    return location.kind === 'server-file'
        ? { kind: 'FILE', file: location.reference }
        : { kind: 'DIRECTORY', directory: location.reference };
}

export function imageLocation(source: components['schemas']['ImageSourceRef']): ImageLocation {
    if (source.kind === 'DEVICE') return { kind: 'device', reference: source.device, displayName: 'Physical device' };
    const reference = source.kind === 'FILE' ? source.file : source.directory;
    return {
        kind: source.kind === 'FILE' ? 'server-file' : 'server-directory',
        reference,
        displayName: reference.relativePath || reference.rootId,
    };
}
