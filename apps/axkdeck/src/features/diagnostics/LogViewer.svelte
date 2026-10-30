<script lang="ts">
    import SwitchControl from '../../lib/components/SwitchControl.svelte';
    import { onMount, tick, untrack } from 'svelte';
    import { on } from 'svelte/events';
    import Icon from '../../lib/components/Icon.svelte';
    import SelectControl from '../../lib/components/SelectControl.svelte';
    import { LogController } from './controller.svelte';
    import { desktopLogs } from './desktop';
    import {
        logSources,
        logLevels,
        type LogLevel,
        type CaptureState,
        type LogDriver,
        type LogSource,
    } from './contracts';
    import LogMultiSelect from './LogMultiSelect.svelte';
    import './logs.css';

    let {
        driver = desktopLogs,
        subscribeVisibility,
        subscribeCapture,
    }: {
        driver?: LogDriver;
        subscribeVisibility?: (callback: (visible: boolean) => void) => Promise<() => void>;
        subscribeCapture?: (callback: (state: CaptureState) => void) => Promise<() => void>;
    } = $props();
    const logs = new LogController({
        read: (request) => driver.read(request),
        clear: () => driver.clear(),
        save: (filter) => driver.save(filter),
        capture: () => driver.capture(),
        setCapture: (level) => driver.setCapture(level),
    });
    let scroller: HTMLDivElement;
    let selectionText = $state('');
    let clipboardError = $state('');
    let saveMenu = $state(false);
    let saveContainer: HTMLDivElement;
    let saveButton: HTMLButtonElement;
    let windowVisible = true;
    function changeLevel(level: LogLevel): void {
        void logs.setLevel(level);
    }
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
        let stopCapture: (() => void) | undefined;
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
        void logs.readCapture();
        if (subscribeCapture)
            void subscribeCapture((state) => logs.updateCapture(state))
                .then((stop) => {
                    if (disposed) stop();
                    else stopCapture = stop;
                })
                .catch((error) => {
                    logs.captureError = String(error);
                });
        if (subscribeVisibility)
            void subscribeVisibility((visible) => {
                windowVisible = visible;
                if (visible) void logs.readCapture();
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
            stopCapture?.();
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
        <LogMultiSelect
            label="Sources"
            options={logSources}
            selected={logs.filter.sources}
            disabled={logs.actionBusy}
            onchange={(values) => logs.setFilter({ sources: values as LogSource[] })}
        />
        <label
            >Level<span class="log-select"
                ><SelectControl
                    label="Level"
                    size="toolbar"
                    value={logs.filter.level}
                    options={logLevels.map((level) => ({
                        value: level,
                        label: level[0].toUpperCase() + level.slice(1),
                    }))}
                    disabled={logs.captureBusy || logs.actionBusy}
                    onchange={changeLevel}
                /></span
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
            <SwitchControl
                label="Word wrap"
                checked={logs.wrap}
                density="diagnostic"
                showLabel
                onchange={(next) => {
                    logs.wrap = next;
                }}
            />
            <SwitchControl
                label="Follow latest"
                checked={logs.follow}
                density="diagnostic"
                showLabel
                onchange={(next) => logs.setFollow(next)}
            />
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
            <div class="log-record" data-level={entry.level}>
                <span class="log-time">{time(entry.timestamp)}</span>{' '}<span class="log-level">{entry.level}</span
                >{' '}<span class="log-source" title={entry.origin === 'localServer' ? 'Local server' : 'Application'}
                    >{entry.source === 'http' ? 'HTTP' : entry.source[0].toUpperCase() + entry.source.slice(1)}</span
                >{' '}<span class="log-text">{entry.text}</span>{'\n'}
            </div>
        {:else}<p class="log-empty">{logs.busy ? 'Loading logs...' : 'No matching log entries'}</p>{/each}
    </div>
    <footer class="log-footer">
        <span
            class:failure={!!(logs.error || clipboardError || logs.captureError)}
            role="status"
            title={logs.error || clipboardError || logs.captureError || logs.status}
            >{logs.error ||
                clipboardError ||
                logs.captureError ||
                (logs.actionBusy ? 'Working...' : logs.status || (logs.follow ? 'Live' : 'Follow paused'))}</span
        >
        <span
            >{logs.captureBusy ? 'Updating level...' : `Level: ${logs.capture.level}`}{logs.capture.localServer !==
            'ready'
                ? ` (local server ${logs.capture.localServer})`
                : ''}</span
        >
        {#if logs.historyChanges}<span title="Some retained log files were rotated out or replaced"
                >History changed</span
            >{/if}
        {#if logs.error}<button type="button" onclick={() => logs.reload()}
                ><Icon name="refresh" size={12} />Retry</button
            >{/if}
    </footer>
</main>
