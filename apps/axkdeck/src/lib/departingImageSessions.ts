import { AxklibApiError } from './httpErrors';

// Record only IDs whose owning page actually departed. A new tab can inherit
// sessionStorage from its opener; live sessions must never appear there.
export class DepartingImageSessions {
    private readonly live = new Set<string>();
    private readonly prefix: string;

    constructor(
        baseUrl: string,
        private readonly close: (id: string, keepalive: boolean) => Promise<void>,
    ) {
        this.prefix = `axkdeck:departed-image:${encodeURIComponent(baseUrl.replace(/\/+$/, ''))}:`;
    }

    track(id: string) {
        if (typeof window === 'undefined') return;
        this.live.add(id);
        window.addEventListener('pagehide', this.depart);
    }

    forget(id: string) {
        this.live.delete(id);
        if (!this.live.size && typeof window !== 'undefined') window.removeEventListener('pagehide', this.depart);
    }

    private readonly depart = (event: PageTransitionEvent) => {
        if (event.persisted) return;
        for (const id of this.live) {
            try {
                window.sessionStorage.setItem(this.prefix + id, '1');
            } catch {
                // A restricted webview can still send the authenticated keepalive.
            }
            void this.release(id, true).catch(() => {});
        }
        this.live.clear();
        window.removeEventListener('pagehide', this.depart);
    };

    async recover() {
        let keys: string[];
        try {
            keys = Object.keys(window.sessionStorage);
        } catch {
            return;
        }
        for (const key of keys) {
            if (!key.startsWith(this.prefix)) continue;
            const id = key.slice(this.prefix.length);
            if (/^image-[a-zA-Z0-9-]+$/.test(id)) await this.release(id, false);
        }
    }

    private async release(id: string, keepalive: boolean) {
        try {
            await this.close(id, keepalive);
        } catch (error) {
            if (!(error instanceof AxklibApiError && error.code === 'image_not_found')) throw error;
        }
        try {
            window.sessionStorage.removeItem(this.prefix + id);
        } catch {
            // Repeating DELETE after a later reload is harmless.
        }
    }
}
