import type { DirectoryLocation, DirectoryRef, FileLocation, ImageLocation } from '../../lib/storageLocations';

export type PickerMode = 'file' | 'directory' | 'save-file' | 'save-directory' | 'media-source';
export type PickerParentDialog =
    | 'audio-import'
    | 'companion-disks'
    | 'package-import'
    | 'package-export'
    | 'audio-export'
    | 'sequence-export'
    | 'media-export'
    | 'sequence-import';
export type PickerSelection = ImageLocation | DirectoryLocation | FileLocation[];

export interface PickerNavigation {
    parentDialog?: PickerParentDialog;
    requireWritableDirectory?: boolean;
    initialDirectory?: DirectoryRef | null;
    ondirectorychange?: (directory: DirectoryRef | null) => void;
}

export interface PickerRequest extends PickerNavigation {
    mode: PickerMode;
    title: string;
    extensions: string[];
    suggestedName: string;
    multiple: boolean;
}

interface PendingPicker {
    request: PickerRequest;
    resolve: (selection: PickerSelection | null) => void;
}

export class PickerController {
    private pending: PendingPicker | null = null;

    constructor(private readonly onChange: (request: PickerRequest | null) => void) {}

    chooseLocation(
        mode: PickerMode,
        title: string,
        extensions: string[] = [],
        suggestedName = '',
        navigation: PickerNavigation = {},
    ): Promise<ImageLocation | DirectoryLocation | null> {
        return this.begin({ mode, title, extensions, suggestedName, multiple: false, ...navigation }, (selection) =>
            Array.isArray(selection) ? null : selection,
        );
    }

    chooseFiles(
        title: string,
        extensions: string[],
        navigation: PickerNavigation = {},
    ): Promise<FileLocation[] | null> {
        return this.begin(
            {
                mode: 'file',
                title,
                extensions,
                suggestedName: '',
                multiple: true,
                ...navigation,
            },
            (selection) => (Array.isArray(selection) ? selection : null),
        );
    }

    finish(selection: PickerSelection | null): void {
        const pending = this.pending;
        this.pending = null;
        this.onChange(null);
        pending?.resolve(selection);
    }

    dispose(): void {
        this.finish(null);
    }

    private begin<T>(request: PickerRequest, convert: (selection: PickerSelection | null) => T): Promise<T> {
        this.finish(null);
        return new Promise((resolve) => {
            this.pending = {
                request,
                resolve: (selection) => resolve(convert(selection)),
            };
            this.onChange(request);
        });
    }
}
