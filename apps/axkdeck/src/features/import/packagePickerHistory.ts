import type { DirectoryRef, FileRef } from '../../lib/storageLocations';

export class PackagePickerHistory {
    lastDirectory: DirectoryRef | null = null;
    lastImportedWorkspaceFile: FileRef | null = null;
    lastImportedLocalPath: string | null = null;
}
