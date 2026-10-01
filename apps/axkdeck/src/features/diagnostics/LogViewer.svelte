<script lang="ts">
    import { onMount, tick, untrack } from 'svelte';
    import { on } from 'svelte/events';
    import Icon from '../../lib/components/Icon.svelte';
    import { LogController } from './controller.svelte';
    import { desktopLogs } from './desktop';
    import type { LogDriver, LogLevel, LogSource } from './contracts';
    import './logs.css';

    let {
        driver = desktopLogs,
        subscribeVisibility,
    }: {
        driver?: LogDriver;
        subscribeVisibility?: (callback: (visible: boolean) => void) => Promise<() => void>;
    } = $props();
    const logs = new LogController({
        read: (request) => driver.read(request),
        clear: () => driver.clear(),
        save: (filter) => driver.save(filter),
    });
    let scroller: HTMLDivElement;
    let selectionText = $state('');
    let clipboardError = $state('');
    let saveMenu = $state(false);
    let saveContainer: HTMLDivElement;
    let saveButton: HTMLButtonElement;
    let windowVisible = true;
    const time = (timestamp: number | null) => {
        const date = new Date(timestamp ?? NaN);
        return Number.isNaN(date.valueOf())
            ? 'Unknown time'
            : date.toISOString().replace('T', ' ').replace('Z', ' UTC');
    };
    function selection(): void {
        const selected = window.getSelection();
        if (selected?.rangeCount && scroller?.contains(selected.anchorNode) && scroller.contains(selected.focusNode)) {
            selectionText = selected.toString();
            if (selectionText) logs.setFollow(false);
        } else selectionText = '';
    }
    async function copy(): Promise<void> {
        if (!selectionText) return;
        clipboardError = '';
        try {
            if (!navigator.clipboard?.writeText) throw new Error('Clipboard access is unavailable');
            await navigator.clipboard.writeText(selectionText);
            logs.status = 'Selection copied';
        } catch (error) {
            clipboardError = String(error);
        }
    }
    function scroll(): void {
        if (scroller.scrollHeight - scroller.scrollTop - scroller.clientHeight > 8) logs.setFollow(false);
    }
    async function toggleSave(): Promise<void> {
        saveMenu = !saveMenu;
        if (saveMenu) {
            await tick();
            saveContainer.querySelector<HTMLButtonElement>('[role="menuitem"]')?.focus();
        }
    }
    function menuKey(event: KeyboardEvent): void {
        if (event.key === 'Escape') {
            event.preventDefault();
            saveMenu = false;
            saveButton.focus();
            return;
        }
        const items = [...saveContainer.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')];
        const index = items.indexOf(document.activeElement as HTMLButtonElement);
        if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
            event.preventDefault();
            items[(index + (event.key === 'ArrowDown' ? 1 : items.length - 1)) % items.length]?.focus();
        }
    }
    function selectAll(event: KeyboardEvent): void {
        if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'a') {
            event.preventDefault();
            const range = document.createRange();
            range.selectNodeContents(scroller);
            const selected = window.getSelection();
            selected?.removeAllRanges();
            selected?.addRange(range);
            selection();
        }
    }
    onMount(() => {
        let disposed = false;
        let unsubscribe: (() => void) | undefined;
        const activate = () => logs.setActive(windowVisible && document.visibilityState !== 'hidden');
        const unselect = on(document, 'selectionchange', selection);
        const unvisible = on(document, 'visibilitychange', activate);
        const unoutside = on(document, 'pointerdown', (event) => {
            if (!saveContainer?.contains(event.target as Node)) saveMenu = false;
        });
        const unfocus = on(document, 'focusin', (event) => {
            if (!saveContainer?.contains(event.target as Node)) saveMenu = false;
        });
        activate();
        if (subscribeVisibility)
            void subscribeVisibility((visible) => {
                windowVisible = visible;
                activate();
            })
                .then((stop) => {
                    if (disposed) stop();
                    else unsubscribe = stop;
                })
                .catch((error) => {
                    logs.setActive(false);
                    logs.error = `Window visibility is unavailable: ${String(error)}`;
                });
        return () => {
            disposed = true;
            logs.setActive(false);
            unselect();
            unvisible();
            unoutside();
            unfocus();
            unsubscribe?.();
        };
    });
    $effect(() => {
        logs.renderRevision;
        untrack(() => {
            selectionText = '';
            if (logs.follow)
                void tick().then(() => {
                    if (scroller && logs.follow) scroller.scrollTop = scroller.scrollHeight;
                });
        });
    });
</script>

<svelte:head><title>axkdeck - Logs</title></svelte:head>
<main class="log-viewer" aria-label="Application logs">
    <div class="log-toolbar" aria-label="Log controls">
        <label
            >Source<span class="log-select"
                ><select
                    value={logs.filter.source ?? ''}
                    disabled={logs.actionBusy}
                    onchange={(event) =>
                        logs.setFilter({ source: (event.currentTarget.value || null) as LogSource | null })}
                >
                    <option value="">All</option><option value="application">Application</option><option
                        value="localServer">Local server</option
                    >
                </select><span aria-hidden="true"><Icon name="chevron" size={12} /></span></span
            ></label
        >
        <label
            >Minimum level<span class="log-select"
                ><select
                    value={logs.filter.minimumLevel ?? ''}
                    disabled={logs.actionBusy}
                    onchange={(event) =>
                        logs.setFilter({ minimumLevel: (event.currentTarget.value || null) as LogLevel | null })}
                >
                    <option value="">All</option><option value="debug">Debug and above</option><option value="info"
                        >Info and above</option
                    ><option value="warning">Warning and above</option><option value="error">Error</option>
                </select><span aria-hidden="true"><Icon name="chevron" size={12} /></span></span
            ></label
        >
        <label class="log-search"
            >Search<input
                type="search"
                value={logs.filter.search}
                maxlength={256}
                disabled={logs.actionBusy}
                oninput={(event) => logs.setFilter({ search: event.currentTarget.value })}
            /></label
        >
        <div class="log-switches">
            <button
                type="button"
                class="log-switch"
                role="switch"
                aria-checked={logs.filter.includeUnclassified}
                disabled={logs.actionBusy}
                onclick={() => logs.setFilter({ includeUnclassified: !logs.filter.includeUnclassified })}
                ><span></span>Include unclassified</button
            >
            <button
                type="button"
                class="log-switch"
                role="switch"
                aria-checked={logs.wrap}
                onclick={() => (logs.wrap = !logs.wrap)}><span></span>Word wrap</button
            >
            <button
                type="button"
                class="log-switch"
                role="switch"
                aria-checked={logs.follow}
                onclick={() => logs.setFollow(!logs.follow)}><span></span>Follow latest</button
            >
        </div>
        <div class="log-actions">
            <button
                type="button"
                class="log-icon"
                aria-label="Clear view"
                title="Clear view without deleting log files"
                disabled={logs.actionBusy}
                onclick={() => logs.clear()}><Icon name="broom" size={16} /></button
            >
            <button
                type="button"
                class="log-icon"
                aria-label="Show retained history"
                title="Show retained history"
                disabled={logs.actionBusy || logs.filter.since === null}
                onclick={() => logs.restoreHistory()}><Icon name="undo" size={16} /></button
            >
            <button
                type="button"
                class="log-icon"
                aria-label="Copy selection"
                title="Copy selection"
                disabled={!selectionText}
                onpointerdown={(event) => event.preventDefault()}
                onclick={copy}><Icon name="copy" size={16} /></button
            >
            <div class="log-save" bind:this={saveContainer}>
                <button
                    type="button"
                    class="log-icon"
                    aria-label="Save logs"
                    title="Save logs"
                    aria-haspopup="menu"
                    aria-expanded={saveMenu}
                    bind:this={saveButton}
                    disabled={logs.actionBusy}
                    onclick={toggleSave}><Icon name="save" size={16} /></button
                >
                {#if saveMenu}
                    <div
                        class="log-save-menu"
                        role="menu"
                        tabindex="-1"
                        aria-label="Save log snapshot"
                        onkeydown={menuKey}
                    >
                        <button
                            type="button"
                            role="menuitem"
                            onclick={() => {
                                saveMenu = false;
                                saveButton.focus();
                                void logs.save(false);
                            }}>Save view...</button
                        >
                        <button
                            type="button"
                            role="menuitem"
                            onclick={() => {
                                saveMenu = false;
                                saveButton.focus();
                                void logs.save(true);
                            }}>Save all logs...</button
                        >
                    </div>
                {/if}
            </div>
        </div>
    </div>
    <div class="log-page-bar">
        <span
            >{logs.total.toLocaleString()} matching entries{logs.filter.since !== null ? ' since Clear view' : ''}</span
        >
        <button
            type="button"
            class="log-icon"
            aria-label="Older entries"
            title="Older entries"
            disabled={logs.busy || !logs.page?.olderCursor}
            onclick={() => logs.older()}><Icon name="undo" size={14} /></button
        >
        <button
            type="button"
            class="log-icon"
            aria-label="Newer entries"
            title="Newer entries"
            disabled={logs.busy || !logs.page?.newerCursor}
            onclick={() => logs.newer()}><Icon name="redo" size={14} /></button
        >
        {#if logs.newCount}<button type="button" class="log-new" onclick={() => logs.setFollow(true)}
                >{logs.newCount} new entries</button
            >{/if}
    </div>
    <div
        class="log-content"
        class:wrap={logs.wrap}
        bind:this={scroller}
        onscroll={scroll}
        onkeydown={selectAll}
        tabindex="0"
        role="textbox"
        aria-readonly="true"
        aria-multiline="true"
        aria-label="Log entries"
    >
        {#each logs.page?.entries ?? [] as entry (entry.id)}
            <div class="log-record" data-level={entry.level ?? 'unclassified'}>
                <span class="log-time">{time(entry.timestamp)}</span>{' '}<span class="log-level"
                    >{entry.level ?? 'unclassified'}</span
                >{' '}<span class="log-source">{entry.source === 'application' ? 'Application' : 'Local server'}</span
                >{' '}<span class="log-text">{entry.text}</span>{'\n'}
            </div>
        {:else}<p class="log-empty">{logs.busy ? 'Loading logs...' : 'No matching log entries'}</p>{/each}
    </div>
    <footer class="log-footer">
        <span
            class:failure={!!(logs.error || clipboardError)}
            role="status"
            title={logs.error || clipboardError || logs.status}
            >{logs.error ||
                clipboardError ||
                (logs.actionBusy ? 'Working...' : logs.status || (logs.follow ? 'Live' : 'Follow paused'))}</span
        >
        {#if logs.historyChanges}<span title="Some retained log files were rotated out or replaced"
                >History changed</span
            >{/if}
        {#if logs.error}<button type="button" onclick={() => logs.reload()}
                ><Icon name="refresh" size={12} />Retry</button
            >{/if}
    </footer>
</main>
