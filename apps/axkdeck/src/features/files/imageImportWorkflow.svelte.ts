import type {
    FilesystemEntry,
    FilesystemImageInspection,
    FilesystemMutationDriver,
    FilesystemRootCapabilities,
} from '../../lib/filesystem';
import type { ClientFilesystemImportEntry, FilesystemImportActions } from '../../lib/filesystemImport';
import type { InputFileLocation } from '../../lib/storageLocations';
import type { JobState } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { FilesImportWorkflow } from './importWorkflow.svelte';
import { inspectedSources, sourceRows } from './importReview';
import { descendants, imageContentRows, selectedImageRows, type ImageImportGroup } from './imageImportSelection';
import { normalizeFilesystemName } from './nameValidation';

export function isFilesystemImageDrop(entries: ClientFilesystemImportEntry[]): boolean {
    return (
        entries.length > 0 &&
        entries.every(
            (entry) => !entry.directory && entry.relativePath.length === 1 && /\.(ima|img)$/i.test(entry.source.name),
        )
    );
}

export class FilesImageImportWorkflow {
    readonly importer: FilesImportWorkflow;
    mode = $state<'File' | 'Contents'>('Contents');
    groups = $state<ImageImportGroup[]>([]);
    fileGroups = $state<ImageImportGroup[]>([]);
    loading = $state(false);
    error = $state('');
    mapping = $state(new Map<string, number>());
    errors = $state(new Map<string, string>());
    private abort: AbortController | null = null;
    private generation = 0;

    constructor(setStatus: (message: string) => void = () => undefined) {
        this.importer = new FilesImportWorkflow(setStatus);
    }
    get visibleGroups(): ImageImportGroup[] {
        return this.mode === 'File' ? this.fileGroups : this.groups;
    }
    get editable(): boolean {
        return !this.loading && this.importer.editable;
    }
    get canReview(): boolean {
        return this.editable && !this.errors.size && this.importer.rows.length <= 10000 && this.importer.canInspect;
    }
    get selectedCount(): number {
        return this.visibleGroups.reduce((count, group) => count + group.selected.length, 0);
    }
    get totalCount(): number {
        return this.visibleGroups.reduce((count, group) => count + group.rows.length, 0);
    }
    get allSelected(): boolean {
        return (
            this.totalCount > 0 &&
            this.visibleGroups.every((group) => {
                const selected = new Set(group.selected);
                const parents = new Set(group.rows.map((row) => row.parent));
                return group.rows.every((_, index) => parents.has(index) || selected.has(index));
            })
        );
    }

    async open(
        revision: number,
        target: FilesystemEntry,
        capabilities: FilesystemRootCapabilities,
        imports: FilesystemImportActions,
        mutations: FilesystemMutationDriver,
        entries: ClientFilesystemImportEntry[],
    ): Promise<void> {
        if (!imports.images || !isFilesystemImageDrop(entries) || this.importer.target) return;
        const images = imports.images;
        let sources: InputFileLocation[] = [];
        const tokens = new Set<string>();
        const release = async () => {
            for (const token of [...tokens]) {
                await images.release(token);
                tokens.delete(token);
            }
            await imports.release(sources);
            sources = [];
        };
        if (!this.importer.open(revision, target, capabilities, { ...imports, release }, mutations)) return;
        this.mode = 'Contents';
        this.groups = [];
        this.fileGroups = [];
        this.error = '';
        this.mapping = new Map();
        this.errors = new Map();
        this.loading = true;
        const generation = ++this.generation;
        const abort = new AbortController();
        this.abort = abort;
        const current = () => generation === this.generation && !!this.importer.target && !abort.signal.aborted;
        const update = (job: JobState) => {
            if (!current()) {
                void imports.cancel(job.jobId).catch(() => undefined);
                return;
            }
            if (job.progress?.label) this.importer.message = job.progress.label;
        };
        try {
            const files = entries.flatMap((entry) => (entry.directory ? [] : [entry.source]));
            if (files.length > 32 || files.some((file) => file.size > 4194304))
                throw new Error('Choose at most 32 floppy images, up to 4 MiB each.');
            this.importer.message = 'Uploading floppy images';
            sources = await imports.upload(files, abort.signal, (message) => {
                if (current()) this.importer.message = message;
            });
            abort.signal.throwIfAborted();
            if (sources.length !== files.length) throw new Error('The uploaded images do not match the selection.');
            const raw = sourceRows(
                sources.map((source, index) => ({ directory: false, relativePath: [files[index].name], source })),
            );
            this.importer.message = 'Inspecting image files';
            const job = await imports.inspectInputs(sources, update);
            abort.signal.throwIfAborted();
            this.fileGroups = inspectedSources(job, raw).map((row, index) => ({
                name: files[index].name,
                rows: [{ ...row, name: normalizeFilesystemName(row.name, capabilities) }],
                selected: [0],
                error: '',
            }));
            for (const [index, source] of sources.entries()) {
                abort.signal.throwIfAborted();
                this.importer.message = `Reading floppy ${index + 1} of ${sources.length}`;
                const group: ImageImportGroup = { name: files[index].name, rows: [], selected: [], error: '' };
                try {
                    const result = await images.inspect(source, update);
                    const inspection = result.result as FilesystemImageInspection | undefined;
                    if (inspection?.inspectionToken) tokens.add(inspection.inspectionToken);
                    abort.signal.throwIfAborted();
                    if (result.status !== 'completed' || !inspection)
                        throw new Error(result.error || 'Unable to read this floppy.');
                    group.rows = imageContentRows(inspection, capabilities);
                    group.selected = group.rows.map((_, entryIndex) => entryIndex);
                } catch (error) {
                    abort.signal.throwIfAborted();
                    group.error = userFacingMessage(error);
                }
                this.groups = [...this.groups, group];
            }
        } catch (error) {
            if (current()) this.error = userFacingMessage(error);
        } finally {
            if (current()) {
                this.loading = false;
                this.rebuild();
                if (this.error) this.importer.message = this.error;
            } else {
                await release().catch(() => undefined);
            }
        }
    }

    setMode(mode: 'File' | 'Contents'): void {
        if (this.editable) {
            this.mode = mode;
            this.rebuild();
        }
    }
    toggle(groupIndex: number, index: number, checked: boolean): void {
        if (!this.editable) return;
        const group = this.visibleGroups[groupIndex];
        const selected = new Set(group.selected);
        for (const child of descendants(group.rows, index)) {
            if (checked) selected.add(child);
            else selected.delete(child);
        }
        // Parent directories are included automatically while any child is selected.
        let parent = group.rows[index].parent;
        while (parent !== null) {
            selected.delete(parent);
            parent = group.rows[parent].parent;
        }
        group.selected = [...selected];
        this.rebuild();
    }
    selection(groupIndex: number, index: number): 'all' | 'some' | 'none' {
        const group = this.visibleGroups[groupIndex];
        const children = descendants(group.rows, index);
        const parents = new Set(children.map((child) => group.rows[child].parent));
        const leaves = children.filter((child) => !parents.has(child));
        const count = leaves.filter((child) => group.selected.includes(child)).length;
        return count === leaves.length ? 'all' : count ? 'some' : 'none';
    }
    selectAll(checked: boolean): void {
        if (!this.editable) return;
        for (const group of this.visibleGroups) group.selected = checked ? group.rows.map((_, index) => index) : [];
        this.rebuild();
    }
    rename(groupIndex: number, index: number, name: string): void {
        if (!this.editable) return;
        this.visibleGroups[groupIndex].rows[index].name = normalizeFilesystemName(name, this.importer.capabilities!);
        this.rebuild();
    }
    conflict(groupIndex: number, index: number, conflict: 'SKIP' | 'REPLACE'): void {
        if (!this.editable) return;
        this.visibleGroups[groupIndex].rows[index].conflict = conflict;
        this.rebuild();
    }
    private rebuild(): void {
        const prepared = selectedImageRows(this.visibleGroups);
        this.mapping = prepared.mapping;
        this.errors = prepared.errors;
        this.importer.setPreparedRows(prepared.rows);
        this.importer.message = prepared.errors.size
            ? 'Resolve duplicate destination names'
            : prepared.rows.length > 10000
              ? 'Choose at most 10000 entries'
              : prepared.rows.length
                ? 'Review the selected entries'
                : 'No entries selected';
    }
    async review(): Promise<void> {
        if (this.canReview) await this.importer.inspect();
    }
    close(): void {
        if (!this.importer.canDismiss) return;
        if (this.importer.phase === 'writing') {
            void this.importer.cancel();
            return;
        }
        this.abort?.abort();
        this.generation += 1;
        this.loading = false;
        this.importer.close();
    }
    dispose(): void {
        this.abort?.abort();
        this.generation += 1;
        this.importer.dispose();
    }
}
