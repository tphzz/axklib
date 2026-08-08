import type { DirectoryListing, FileRef, SandboxEntry } from './storageLocations';

export interface RestoredStorageListing {
    listing: DirectoryListing;
    activeOptionIndex: number | null;
}

export function storageEntryIsVisible(
    entry: SandboxEntry,
    filesAreSelectable: boolean,
    normalizedExtensions: string[],
): boolean {
    if (entry.kind === 'DIRECTORY') return true;
    if (!filesAreSelectable) return false;
    if (normalizedExtensions.length === 0) return true;
    const extension = entry.name.split('.').pop()?.toLocaleLowerCase() ?? '';
    return normalizedExtensions.includes(extension);
}

export async function restoreRememberedFile(
    initialListing: DirectoryListing,
    rememberedFile: FileRef | null,
    entryIsVisible: (entry: SandboxEntry) => boolean,
    loadPage: (cursor: string) => Promise<DirectoryListing>,
): Promise<RestoredStorageListing> {
    if (!rememberedFile || !belongsToDirectory(rememberedFile, initialListing.directory)) {
        return { listing: initialListing, activeOptionIndex: null };
    }

    let listing = initialListing;
    const visitedCursors = new Set<string>();
    while (!containsFile(listing.entries, rememberedFile) && listing.nextCursor) {
        const cursor = listing.nextCursor;
        if (visitedCursors.has(cursor)) break;
        visitedCursors.add(cursor);
        const page = await loadPage(cursor);
        listing = {
            ...page,
            directory: initialListing.directory,
            entries: [...listing.entries, ...page.entries],
        };
    }

    const activeOptionIndex = listing.entries
        .filter(entryIsVisible)
        .findIndex((entry) => entry.kind === 'FILE' && entry.relativePath === rememberedFile.relativePath);
    return { listing, activeOptionIndex: activeOptionIndex >= 0 ? activeOptionIndex : null };
}

function belongsToDirectory(file: FileRef, directory: FileRef): boolean {
    if (file.rootId !== directory.rootId) return false;
    const separator = file.relativePath.lastIndexOf('/');
    const parentPath = separator >= 0 ? file.relativePath.slice(0, separator) : '';
    return parentPath === directory.relativePath;
}

function containsFile(entries: SandboxEntry[], file: FileRef): boolean {
    return entries.some((entry) => entry.kind === 'FILE' && entry.relativePath === file.relativePath);
}
