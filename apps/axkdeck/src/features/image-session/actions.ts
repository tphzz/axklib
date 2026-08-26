import type { ImageLocation } from '../../lib/storageLocations';
import type { ImageTransport, JobState, OpenedImage } from '../../lib/transport';

type SessionTransport = Pick<ImageTransport, 'openImage' | 'keepImageAlive' | 'refreshImage' | 'closeImage'>;

export interface ImageSessionSnapshot {
    sessionId: number | null;
    location: ImageLocation | null;
    opening: boolean;
}

export class ImageSessionController {
    private generation = 0;
    private snapshotValue: ImageSessionSnapshot = { sessionId: null, location: null, opening: false };
    private disposed = false;
    private activeOpen: AbortController | null = null;

    constructor(
        private readonly transport: SessionTransport,
        private readonly invalidateSession: (sessionId: number) => Promise<void>,
        private readonly onChange: (snapshot: ImageSessionSnapshot) => void,
    ) {}

    snapshot(): ImageSessionSnapshot {
        return this.snapshotValue;
    }

    async open(location: ImageLocation, onUpdate?: (job: JobState) => void): Promise<OpenedImage | null> {
        if (this.disposed) throw new Error('Image session controller is disposed');
        this.activeOpen?.abort();
        const abortController = new AbortController();
        this.activeOpen = abortController;
        const generation = ++this.generation;
        const previousSessionId = this.snapshotValue.sessionId;
        this.update({ ...this.snapshotValue, opening: true });
        let candidateSessionId: number | null = null;
        try {
            const opened = await this.transport.openImage(location, {
                signal: abortController.signal,
                onUpdate,
            });
            candidateSessionId = opened.sessionId;
            if (!this.isCurrent(generation)) {
                await this.transport.closeImage(opened.sessionId);
                return null;
            }
            if (previousSessionId !== null) {
                await this.invalidateSession(previousSessionId);
                await this.transport.closeImage(previousSessionId);
            }
            if (!this.isCurrent(generation)) {
                await this.transport.closeImage(opened.sessionId);
                return null;
            }
            candidateSessionId = null;
            this.update({ sessionId: opened.sessionId, location, opening: false });
            return opened;
        } catch (error) {
            if (candidateSessionId !== null) {
                await this.transport.closeImage(candidateSessionId).catch(() => undefined);
            }
            if (this.isCurrent(generation)) this.update({ ...this.snapshotValue, opening: false });
            throw error;
        } finally {
            if (this.activeOpen === abortController) this.activeOpen = null;
        }
    }

    cancelOpen(): void {
        this.activeOpen?.abort();
    }

    async refresh(): Promise<OpenedImage | null> {
        const sessionId = this.snapshotValue.sessionId;
        if (sessionId === null || this.disposed) return null;
        const opened = await this.transport.refreshImage(sessionId);
        return this.snapshotValue.sessionId === sessionId && !this.disposed ? opened : null;
    }

    async keepAlive(): Promise<boolean> {
        const sessionId = this.snapshotValue.sessionId;
        if (sessionId === null || this.disposed) return false;
        await this.transport.keepImageAlive(sessionId);
        return this.snapshotValue.sessionId === sessionId && !this.disposed;
    }

    async close(): Promise<void> {
        this.activeOpen?.abort();
        ++this.generation;
        const sessionId = this.snapshotValue.sessionId;
        this.update({ ...this.snapshotValue, opening: false });
        if (sessionId === null) return;
        await this.invalidateSession(sessionId);
        await this.transport.closeImage(sessionId);
        if (this.snapshotValue.sessionId === sessionId) {
            this.update({ sessionId: null, location: null, opening: false });
        }
    }

    async dispose(): Promise<void> {
        if (this.disposed) return;
        this.disposed = true;
        await this.close();
    }

    private isCurrent(generation: number): boolean {
        return !this.disposed && generation === this.generation;
    }

    private update(snapshot: ImageSessionSnapshot): void {
        this.snapshotValue = snapshot;
        this.onChange(snapshot);
    }
}
