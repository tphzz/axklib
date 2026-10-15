import type { AudioIdentity } from '../transport';

export function sameAudioIdentity(a: AudioIdentity, b: AudioIdentity): boolean {
    return a.imageId === b.imageId && a.documentId === b.documentId && a.revision === b.revision;
}

export function audioIdentityKey(identity: AudioIdentity): string {
    return JSON.stringify([identity.imageId, identity.documentId, identity.revision]);
}
