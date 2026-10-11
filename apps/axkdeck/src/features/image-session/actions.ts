import type { ImageLocation } from '../../lib/storageLocations';
import type { ImageTransport, JobState, OpenedImage } from '../../lib/transport';
import { userFacingMessage } from '../../lib/userFacingMessage';

type SessionTransport = Pick<ImageTransport, 'openImage' | 'keepImageAlive' | 'refreshImage' | 'closeImage'>;
export type ImageSessionPreparation = (
    candidate: OpenedImage,
    onProgress?: (progress: NonNullable<JobState['progress']>) => void,
) => Promise<void>;

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
    private readonly pendingOpenCleanup = new Set<number>();

    constructor(
        private readonly transport: SessionTransport,
        private readonly invalidateSession: (sessionId: number) => Promise<void>,
        private readonly onChange: (snapshot: ImageSessionSnapshot) => void,
    ) {}

    snapshot(): ImageSessionSnapshot {
        return this.snapshotValue;
    }

    async open(
        location: ImageLocation,
        onUpdate?: (job: JobState) => void,
        prepare?: ImageSessionPreparation,
    ): Promise<OpenedImage | null> {
        if (this.disposed) throw new Error('Image session controller is disposed');
        if (this.pendingOpenCleanup.size) await this.cleanupOpen();
        this.activeOpen?.abort();
        const abortController = new AbortController();
        this.activeOpen = abortController;
        const generation = ++this.generation;
        const previousSessionId = this.snapshotValue.sessionId;
        this.update({ ...this.snapshotValue, opening: true });
        let candidateSessionId: number | null = null;
        let failure: unknown;
        const current = () => {
            if (!this.isCurrent(generation)) return false;
            abortController.signal.throwIfAborted();
            return true;
        };
        try {
            let opened = await this.transport.openImage(location, {
                signal: abortController.signal,
                onUpdate,
            });
            candidateSessionId = opened.sessionId;
            if (!current()) return null;
            if (prepare) {
                await prepare(opened);
                if (!current()) return null;
                opened = await this.transport.refreshImage(candidateSessionId);
                if (opened.sessionId !== candidateSessionId) throw new Error('Prepared image session changed');
                if (!current()) return null;
            }
            if (previousSessionId !== null) {
                await this.invalidateSession(previousSessionId);
                await this.transport.closeImage(previousSessionId);
            }
            if (!current()) return null;
            candidateSessionId = null;
            this.update({ sessionId: opened.sessionId, location, opening: false });
            return opened;
        } catch (error) {
            failure = error;
            throw error;
        } finally {
            try {
                if (candidateSessionId !== null) {
                    this.pendingOpenCleanup.add(candidateSessionId);
                    await this.cleanupOpen();
                }
            } catch (error) {
                throw new Error(
                    `${failure ? `${userFacingMessage(failure)}. ` : ''}Connection cleanup failed: ${userFacingMessage(error)}`,
                );
            } finally {
                if (this.isCurrent(generation) && this.snapshotValue.opening)
                    this.update({ ...this.snapshotValue, opening: false });
                if (this.activeOpen === abortController) this.activeOpen = null;
            }
        }
    }

    async cleanupOpen(): Promise<void> {
        for (const sessionId of this.pendingOpenCleanup) {
            await this.transport.closeImage(sessionId);
            this.pendingOpenCleanup.delete(sessionId);
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
        await this.cleanupOpen();
    }

    private isCurrent(generation: number): boolean {
        return !this.disposed && generation === this.generation;
    }

    private update(snapshot: ImageSessionSnapshot): void {
        this.snapshotValue = snapshot;
        this.onChange(snapshot);
    }
}
