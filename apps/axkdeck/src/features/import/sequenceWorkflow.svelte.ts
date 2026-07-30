import { browserUploadSource, type ClientUploadSource } from '../../lib/clientUploadSource';
import { midiExtensions, midiMediaType } from '../../lib/midiImport';
import type { DirectoryRef, FileLocation, ImageLocation } from '../../lib/storageLocations';
import type { ImageTransport, SequenceImportItem, SequenceImportTarget } from '../../lib/transport';
import type { SequenceItem, WorkspaceView } from '../../lib/types';
import { userFacingMessage } from '../../lib/userFacingMessage';
import type { PickerController } from '../dialogs/picker';
import type { JobController } from '../jobs/actions';

export interface SequenceImportRequest {
    files: (ClientUploadSource | FileLocation)[];
    target: SequenceImportTarget;
}

interface SequenceImportDependencies {
    transport: ImageTransport;
    jobs: JobController;
    picker: PickerController;
    sessionId: () => number | null;
    imageLocation: () => ImageLocation | null;
    mutationsAvailable: () => boolean;
    selectedVolume: () => { partitionIndex: number; volumeName: string } | null;
    sequences: () => SequenceItem[];
    refreshSession: (preferred: { partitionIndex: number; volumeName?: string }) => Promise<void>;
    invalidateSession: (sessionId: number) => Promise<void>;
    selectWorkspace: (view: WorkspaceView) => void;
    selectSequence: (sequence: SequenceItem) => void;
    setStatus: (status: string) => void;
    reportTiming: (operation: string, started: number, itemCount: number) => void;
}

export class SequenceImportWorkflow {
    request = $state<SequenceImportRequest | null>(null);
    private lastDirectory = $state<DirectoryRef | null>(null);

    constructor(private readonly dependencies: SequenceImportDependencies) {}

    activeTarget(): SequenceImportTarget | null {
        return this.dependencies.mutationsAvailable() ? this.dependencies.selectedVolume() : null;
    }

    chooseFiles(): void {
        const target = this.activeTarget();
        if (!target || !this.dependencies.imageLocation()) {
            this.dependencies.setStatus('Select a writable volume first');
            return;
        }
        this.request = { files: [], target };
    }

    filesChosen(event: Event): void {
        const input = event.currentTarget as HTMLInputElement;
        this.requestFiles(
            Array.from(input.files ?? []).map(browserUploadSource),
            this.request?.target ?? this.activeTarget(),
        );
        input.value = '';
    }

    async chooseWorkspace(): Promise<void> {
        const request = this.request;
        if (!request) return;
        const selections = await this.dependencies.picker.chooseFiles('Choose MIDI files', [...midiExtensions], {
            parentDialog: 'sequence-import',
            initialDirectory: this.lastDirectory,
            ondirectorychange: (directory) => (this.lastDirectory = directory),
        });
        if (!selections || !this.request) return;
        this.requestFiles(selections, request.target);
    }

    chooseLocal(input: HTMLInputElement): void {
        if (!this.request || !this.dependencies.transport.supportsClientUploads) return;
        input.click();
    }

    async commit(items: SequenceImportItem[]): Promise<void> {
        const request = this.request;
        const sessionId = this.dependencies.sessionId();
        if (!request || sessionId === null) throw new Error('MIDI import target is no longer available');
        const target = request.target;
        const firstName = items[0]?.sequenceName;
        const started = performance.now();
        this.dependencies.setStatus('Importing MIDI');
        try {
            await this.dependencies.invalidateSession(sessionId);
            const completed = await this.dependencies.jobs.run(
                () => this.dependencies.transport.startSequenceImport(sessionId, target, items),
                (update) => {
                    if (update.progress?.label) this.dependencies.setStatus(update.progress.label);
                },
            );
            if (completed.status !== 'completed') throw new Error(completed.error ?? 'MIDI import did not complete');
            this.dependencies.selectWorkspace('sequences');
            await this.dependencies.refreshSession(target);
            const inserted = this.dependencies.sequences().find((sequence) => sequence.name === firstName);
            if (inserted) this.dependencies.selectSequence(inserted);
            this.dependencies.reportTiming('midi-import', started, items.length);
        } catch (error) {
            this.dependencies.setStatus(userFacingMessage(error));
            throw error;
        }
    }

    private requestFiles(files: (ClientUploadSource | FileLocation)[], target: SequenceImportTarget | null): void {
        const admitted = files
            .map((source) => {
                const mediaType = midiMediaType(sourceName(source));
                if (!mediaType) return null;
                return 'kind' in source ? source : { ...source, type: mediaType };
            })
            .filter((source) => source !== null);
        if (files.length > 0 && admitted.length === 0) {
            this.dependencies.setStatus('No Standard MIDI Files were selected');
            return;
        }
        if (!target || admitted.length === 0 || !this.dependencies.imageLocation()) {
            this.dependencies.setStatus(target ? 'Choose MIDI files' : 'Select a writable volume first');
            return;
        }
        this.request = { files: admitted, target };
    }
}

function sourceName(source: ClientUploadSource | FileLocation): string {
    return 'kind' in source ? (source.reference.relativePath.split('/').at(-1) ?? source.displayName) : source.name;
}
