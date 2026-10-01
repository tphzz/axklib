import { defaultLogFilter, type LogDriver, type LogFilter, type LogPage } from './contracts';

type Navigation = { before?: number | null; after?: number | null };
const message = (error: unknown) => (error instanceof Error ? error.message : String(error));

export class LogController {
    filter = $state(defaultLogFilter());
    page = $state<LogPage | null>(null);
    follow = $state(true);
    wrap = $state(false);
    busy = $state(false);
    actionBusy = $state(false);
    error = $state('');
    status = $state('');
    newCount = $state(0);
    total = $state(0);
    historyChanges = $state(0);
    renderRevision = $state(0);
    private active = false;
    private version = 0;
    private knownThrough = 0;
    private reading: Promise<void> | null = null;
    private pending: Navigation | null = null;
    private timer: ReturnType<typeof setTimeout> | undefined;

    constructor(private readonly driver: LogDriver) {}

    setActive(active: boolean): void {
        if (active === this.active) return;
        this.active = active;
        this.version++;
        this.cancelTimer();
        if (active) {
            if (!this.page || this.follow) this.pending = {};
            this.pump();
        }
    }

    setFilter(change: Partial<LogFilter>): void {
        this.filter = { ...this.filter, ...change };
        this.reload();
    }

    setFollow(follow: boolean): void {
        this.follow = follow;
        if (follow) this.reload();
        else {
            this.version++;
            this.pending = null;
        }
    }

    reload(navigation: Navigation = {}): void {
        this.version++;
        this.error = '';
        this.pending = navigation;
        this.cancelTimer();
        this.pump();
    }

    older(): void {
        if (!this.page?.olderCursor) return;
        this.follow = false;
        this.reload({ before: this.page.olderCursor });
    }

    newer(): void {
        if (!this.page?.newerCursor) return;
        this.follow = false;
        this.reload({ after: this.page.newerCursor });
    }

    restoreHistory(): void {
        this.filter = { ...this.filter, since: null };
        this.follow = true;
        this.status = 'Retained history restored';
        this.reload();
    }

    async clear(): Promise<void> {
        await this.action(async () => {
            const since = await this.driver.clear();
            this.filter = { ...this.filter, since };
            this.follow = true;
            this.page = null;
            this.newCount = 0;
            this.status = 'View cleared';
            this.renderRevision++;
            this.pending = {};
        });
    }

    async save(all: boolean): Promise<void> {
        const filter = all ? null : { ...this.filter };
        await this.action(async () => {
            const result = await this.driver.save(filter);
            if (result) this.status = result.warning ?? `Saved ${result.path}`;
        });
    }

    private async action(operation: () => Promise<void>): Promise<void> {
        if (this.actionBusy) return;
        this.actionBusy = true;
        this.cancelTimer();
        try {
            await this.reading;
            this.error = '';
            await operation();
        } catch (error) {
            this.error = message(error);
        } finally {
            this.actionBusy = false;
            this.pump();
        }
    }

    private cancelTimer(): void {
        clearTimeout(this.timer);
        this.timer = undefined;
    }

    private pump(): void {
        if (!this.active || this.reading || this.actionBusy || this.error) return;
        const navigation = this.pending;
        this.pending = null;
        this.reading = this.read(navigation).finally(() => {
            this.reading = null;
            if (!this.active || this.actionBusy || this.error) return;
            if (this.pending) this.pump();
            else this.timer = setTimeout(() => this.pump(), 500);
        });
    }

    private async read(navigation: Navigation | null): Promise<void> {
        const version = this.version;
        const metadataOnly = !navigation && !this.follow;
        this.busy = true;
        try {
            const page = await this.driver.read({
                filter: { ...this.filter },
                before: navigation?.before ?? null,
                after: navigation?.after ?? null,
                knownThrough: this.knownThrough,
                metadataOnly,
            });
            if (version !== this.version || !this.active) return;
            this.total = page.total;
            this.historyChanges = page.historyChanges;
            if (navigation || (this.follow && !metadataOnly)) {
                this.page = page;
                this.knownThrough = page.sequence;
                this.newCount = 0;
                this.renderRevision++;
            } else this.newCount = page.newCount;
        } catch (error) {
            if (version === this.version && this.active) this.error = message(error);
        } finally {
            this.busy = false;
        }
    }
}
