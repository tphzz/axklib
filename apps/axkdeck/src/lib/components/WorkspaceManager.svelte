<script lang="ts">
    import { onMount } from 'svelte';
    import { invoke } from '@tauri-apps/api/core';
    import {
        AxklibHttpApiClient,
        type HostDirectoryListing,
        type HostDirectoryRoot,
        type WorkspaceSnapshot,
    } from '../httpApiClient';
    import { reportError } from '../diagnostics';
    import { userFacingMessage } from '../userFacingMessage';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        open: boolean;
        onclose: () => void;
    }

    let { open, onclose }: Props = $props();
    const connection = window.__AXKLIB_SERVER__;
    const client = connection ? new AxklibHttpApiClient(connection) : null;
    const local = connection?.mode === 'local';
    let snapshot = $state<WorkspaceSnapshot | null>(null);
    let loading = $state(true);
    let choosing = $state(false);
    let error = $state('');
    let adding = $state(false);
    let selectedPath = $state('');
    let candidateId = $state('');
    let displayName = $state('');
    let writable = $state(true);
    let hostRoots = $state<HostDirectoryRoot[]>([]);
    let hostListing = $state<HostDirectoryListing | null>(null);

    const forced = $derived(snapshot?.state === 'NO_AVAILABLE_WORKSPACE' || snapshot?.state === 'CONFIGURATION_ERROR');
    const visible = $derived(open || forced);

    onMount(() => {
        const revalidate = (): void => void refresh();
        window.addEventListener('focus', revalidate);
        void refresh();
        return () => window.removeEventListener('focus', revalidate);
    });

    async function refresh(): Promise<void> {
        if (!client) {
            loading = false;
            return;
        }
        loading = true;
        error = '';
        try {
            snapshot = await client.workspaces();
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            loading = false;
        }
    }

    async function beginAdd(): Promise<void> {
        error = '';
        selectedPath = '';
        candidateId = '';
        displayName = '';
        writable = true;
        if (local) {
            choosing = true;
            try {
                const candidate = await invoke<{ candidateId: string; suggestedName: string } | null>(
                    'select_local_workspace',
                );
                if (!candidate) return;
                candidateId = candidate.candidateId;
                displayName = candidate.suggestedName;
                adding = true;
            } catch (reason) {
                error = userFacingMessage(reason);
                reportError('Choose local storage location failed', reason);
            } finally {
                choosing = false;
            }
            return;
        }
        try {
            hostRoots = await client!.hostDirectoryRoots();
            hostListing = null;
            adding = true;
        } catch (reason) {
            error = userFacingMessage(reason);
        }
    }

    async function browse(path: string): Promise<void> {
        loading = true;
        error = '';
        try {
            hostListing = await client!.hostDirectoryListing(path);
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            loading = false;
        }
    }

    function chooseRemoteDirectory(path: string): void {
        selectedPath = path;
        const segments = path.replace(/[\\/]+$/, '').split(/[\\/]/);
        displayName = segments.at(-1) || 'Storage location';
    }

    function cancelAdd(): void {
        adding = false;
        selectedPath = '';
        candidateId = '';
        displayName = '';
        hostListing = null;
        error = '';
    }

    async function saveWorkspace(): Promise<void> {
        if (!snapshot || !displayName.trim()) {
            error = 'Enter a storage location name';
            return;
        }
        loading = true;
        error = '';
        try {
            if (local) {
                await invoke('commit_local_workspace', {
                    candidateId,
                    displayName: displayName.trim(),
                    writable,
                    revision: snapshot.revision,
                });
            } else {
                if (!selectedPath) throw new Error('Select a directory');
                await client!.createWorkspace({
                    displayName: displayName.trim(),
                    path: selectedPath,
                    writable,
                    revision: snapshot.revision,
                });
            }
            adding = false;
            await refresh();
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            loading = false;
        }
    }

    async function removeWorkspace(id: string): Promise<void> {
        if (!snapshot) return;
        loading = true;
        error = '';
        try {
            await client!.removeWorkspace(id, snapshot.revision);
            await refresh();
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            loading = false;
        }
    }

    async function resetConfiguration(): Promise<void> {
        loading = true;
        error = '';
        try {
            await client!.resetWorkspaceStore();
            await refresh();
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            loading = false;
        }
    }

    function close(): void {
        if (!forced) onclose();
    }
</script>

{#if visible && connection}
    <div class="dialog-backdrop dialog-backdrop-top" role="presentation">
        <div
            class="dialog-shell workspace-dialog"
            role="dialog"
            aria-modal="true"
            aria-label="Storage locations"
            use:modal={{ onescape: close }}
        >
            <header class="dialog-header">
                <h2>Storage locations</h2>
                {#if !forced}<button class="icon-button" type="button" aria-label="Close" onclick={close}>×</button
                    >{/if}
            </header>

            {#if snapshot?.state === 'NO_AVAILABLE_WORKSPACE'}
                <p class="workspace-notice">Choose a directory before opening or creating sampler images.</p>
            {:else if snapshot?.state === 'CONFIGURATION_ERROR'}
                <div class="workspace-error" role="alert">
                    <strong>Storage location configuration cannot be read</strong>
                    <span>{snapshot.configurationIssue}</span>
                    <button class="secondary-button" type="button" onclick={() => void resetConfiguration()}
                        >Archive and reset</button
                    >
                </div>
            {/if}

            {#if snapshot?.state !== 'CONFIGURATION_ERROR'}
                {#if (snapshot?.workspaces.length ?? 0) > 0}
                    <div
                        class="workspace-list"
                        role="list"
                        aria-label="Configured storage locations"
                        aria-busy={loading}
                    >
                        {#each snapshot?.workspaces ?? [] as workspace (workspace.id)}
                            <div class="workspace-row" role="listitem">
                                <Icon name="folder" size={17} />
                                <span>
                                    <strong>{workspace.displayName}</strong>
                                    <small
                                        >{workspace.status === 'AVAILABLE'
                                            ? workspace.effectiveWritable
                                                ? 'Writable'
                                                : 'Read-only'
                                            : workspace.issue}</small
                                    >
                                </span>
                                <button
                                    class="icon-button"
                                    type="button"
                                    aria-label={`Remove ${workspace.displayName}`}
                                    title="Remove storage location"
                                    onclick={() => void removeWorkspace(workspace.id)}>×</button
                                >
                            </div>
                        {/each}
                    </div>
                {/if}

                {#if adding && !local && !selectedPath}
                    <div class="host-browser">
                        {#if hostListing}
                            <div class="host-location">
                                <button
                                    class="icon-button"
                                    type="button"
                                    aria-label="Parent directory"
                                    disabled={!hostListing.parentPath}
                                    onclick={() => hostListing?.parentPath && void browse(hostListing.parentPath)}
                                    >←</button
                                >
                                <span>{hostListing.path}</span>
                                <button
                                    class="secondary-button"
                                    type="button"
                                    onclick={() => chooseRemoteDirectory(hostListing!.path)}>Select</button
                                >
                            </div>
                            {#each hostListing.entries as entry (entry.path)}
                                <button class="host-row" type="button" onclick={() => void browse(entry.path)}
                                    ><Icon name="folder" size={16} /><span>{entry.name}</span><Icon
                                        name="chevron"
                                        size={14}
                                    /></button
                                >
                            {/each}
                        {:else}
                            {#each hostRoots as root (root.path)}
                                <button class="host-row" type="button" onclick={() => void browse(root.path)}
                                    ><Icon name="folder" size={16} /><span>{root.name}</span><Icon
                                        name="chevron"
                                        size={14}
                                    /></button
                                >
                            {/each}
                        {/if}
                    </div>
                {/if}
            {/if}

            {#if error && !(adding && (local || selectedPath))}<p class="workspace-error" role="alert">{error}</p>{/if}
            {#if (snapshot?.state !== 'CONFIGURATION_ERROR' && !adding) || !forced}
                <footer class="dialog-footer">
                    {#if snapshot?.state !== 'CONFIGURATION_ERROR' && !adding}
                        <button
                            class="primary-button"
                            type="button"
                            disabled={loading || choosing}
                            onclick={() => void beginAdd()}
                            >{choosing ? 'Choosing folder…' : 'Add storage location'}</button
                        >
                    {/if}
                    {#if !forced}<button class="secondary-button" type="button" onclick={close}>Close</button>{/if}
                </footer>
            {/if}
        </div>
    </div>

    {#if adding && (local || selectedPath)}
        <div class="dialog-backdrop dialog-backdrop-raised workspace-confirm-backdrop" role="presentation">
            <div
                class="dialog-shell workspace-confirm-dialog"
                role="dialog"
                aria-modal="true"
                aria-label="Add storage location"
                use:modal={{ onescape: cancelAdd }}
            >
                <header class="dialog-header">
                    <h2>Add storage location</h2>
                    <button class="icon-button" type="button" aria-label="Close" onclick={cancelAdd}>×</button>
                </header>
                <div class="workspace-form">
                    {#if selectedPath}<p class="selected-path" title={selectedPath}>{selectedPath}</p>{/if}
                    <label>Location name<input type="text" bind:value={displayName} /></label>
                    <label class="workspace-toggle"
                        ><input type="checkbox" bind:checked={writable} /> Allow image creation and changes</label
                    >
                    {#if error}<p class="workspace-error" role="alert">{error}</p>{/if}
                </div>
                <footer class="dialog-footer">
                    <button class="secondary-button" type="button" onclick={cancelAdd}>Cancel</button>
                    <button class="primary-button" type="button" disabled={loading} onclick={() => void saveWorkspace()}
                        >Add storage location</button
                    >
                </footer>
            </div>
        </div>
    {/if}
{/if}

<style>
    .workspace-dialog {
        width: min(560px, calc(100vw - 32px));
    }
    .workspace-confirm-backdrop {
        z-index: 90;
    }
    .workspace-confirm-dialog {
        width: min(400px, calc(100vw - 32px));
    }
    .host-location {
        display: flex;
        align-items: center;
        gap: 8px;
    }
    .workspace-notice,
    .workspace-error {
        margin: 10px 12px;
        padding: 8px 10px;
        border: 1px solid rgb(104 151 187 / 28%);
        border-radius: 6px;
        background: rgb(104 151 187 / 8%);
    }
    .workspace-notice {
        color: var(--color-text);
    }
    .workspace-error {
        color: #f1a4a4;
        border-color: rgb(190 80 80 / 35%);
        background: rgb(120 35 35 / 20%);
    }
    .workspace-list,
    .host-browser {
        min-height: 72px;
        overflow: auto;
        padding: 6px 12px;
    }
    .workspace-list {
        max-height: 200px;
    }
    .workspace-row,
    .host-row {
        min-height: 36px;
        display: grid;
        grid-template-columns: 20px 1fr auto;
        align-items: center;
        gap: 8px;
        border-bottom: 1px solid var(--color-border);
    }
    .workspace-row span,
    .workspace-row small {
        display: block;
    }
    .workspace-row strong {
        font-size: 10px;
    }
    .workspace-row small {
        margin-top: 1px;
        color: var(--color-text-muted);
        font-size: 8px;
    }
    .host-row {
        width: 100%;
        color: inherit;
        text-align: left;
        background: transparent;
        border: 0;
        border-bottom: 1px solid var(--color-border);
    }
    .host-location {
        position: sticky;
        top: 0;
        padding: 5px 0;
        background: var(--color-panel);
    }
    .host-location span {
        min-width: 0;
        flex: 1;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .workspace-form {
        display: grid;
        gap: 9px;
        padding: 10px 12px;
    }
    .selected-path {
        margin: 0;
        overflow: hidden;
        color: var(--color-text-muted);
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    .workspace-form label {
        display: grid;
        gap: 4px;
        color: var(--color-text-muted);
        font-size: 10px;
    }
    .workspace-form input[type='text'] {
        width: 100%;
        min-height: 30px;
        padding: 0 10px;
        color: var(--color-text-strong);
        border: 1px solid var(--color-border);
        border-radius: 6px;
        outline: none;
        background: var(--color-bg-deep);
    }
    .workspace-form input[type='text']:focus {
        border-color: rgb(76 151 191 / 70%);
    }
    .workspace-toggle {
        grid-template-columns: auto 1fr !important;
        align-items: center;
    }
</style>
