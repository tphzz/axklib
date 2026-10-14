<script lang="ts">
    import { tick } from 'svelte';
    import type { DirectoryRef, SandboxRoot } from '../storageLocations';
    import Icon from './Icon.svelte';

    let {
        root,
        directory,
        loading,
        folderSelected = false,
        onhome,
        onup,
        onbreadcrumb,
        oncreate,
        ontogglefolder,
    }: {
        root: SandboxRoot | null;
        directory: DirectoryRef | null;
        loading: boolean;
        folderSelected?: boolean;
        onhome: () => void;
        onup: () => void;
        onbreadcrumb: (reference: DirectoryRef) => void;
        oncreate?: () => void;
        ontogglefolder?: () => void;
    } = $props();
    let element = $state<HTMLOListElement | null>(null);
    const breadcrumbs = $derived.by(() => {
        if (!root || !directory) return [{ label: 'Workspaces', reference: null }];
        const current = directory;
        return [
            { label: root.displayName, reference: { rootId: root.id, relativePath: '' } },
            ...current.relativePath
                .split('/')
                .filter(Boolean)
                .map((label, index, parts) => ({
                    label,
                    reference: { rootId: current.rootId, relativePath: parts.slice(0, index + 1).join('/') },
                })),
        ];
    });
    $effect(() => {
        breadcrumbs;
        void tick().then(() =>
            element
                ?.querySelector<HTMLElement>('[aria-current="location"]')
                ?.scrollIntoView?.({ block: 'nearest', inline: 'nearest' }),
        );
    });
</script>

<nav class="storage-picker-location" aria-label="Storage location">
    <button
        class="icon-button"
        type="button"
        aria-label="Go to all workspaces"
        title="All workspaces"
        disabled={!root || loading}
        onclick={onhome}
    >
        <Icon name="home" size={15} />
    </button>
    <button
        class="icon-button"
        type="button"
        aria-label="Parent directory"
        title="Parent directory"
        disabled={!root || !directory || loading}
        onclick={onup}
    >
        <Icon name="chevron" size={14} class="storage-picker-parent-icon" />
    </button>
    <ol class="storage-picker-breadcrumbs" bind:this={element}>
        {#each breadcrumbs as breadcrumb, index (`${breadcrumb.reference?.rootId ?? 'home'}:${breadcrumb.reference?.relativePath ?? ''}`)}
            <li class="storage-picker-breadcrumb-item">
                {#if index > 0}<Icon name="chevron" size={11} class="storage-picker-breadcrumb-separator" />{/if}
                {#if index === breadcrumbs.length - 1}
                    <span class="storage-picker-breadcrumb-current" aria-current="location" title={breadcrumb.label}
                        >{breadcrumb.label}</span
                    >
                {:else if breadcrumb.reference}
                    <button
                        class="storage-picker-breadcrumb"
                        type="button"
                        disabled={loading}
                        title={breadcrumb.label}
                        onclick={() => onbreadcrumb(breadcrumb.reference!)}>{breadcrumb.label}</button
                    >
                {/if}
            </li>
        {/each}
    </ol>
    <div class="storage-picker-location-actions">
        {#if directory && ontogglefolder}
            <button
                class="icon-button"
                type="button"
                disabled={loading}
                aria-pressed={folderSelected}
                aria-label={folderSelected ? 'Remove current folder' : 'Add current folder'}
                title={folderSelected ? 'Remove current folder' : 'Add current folder'}
                onclick={ontogglefolder}
            >
                <Icon name={folderSelected ? 'check' : 'folder-plus'} size={15} />
            </button>
        {/if}
        {#if directory && oncreate}
            <button
                class="secondary-button storage-picker-directory-action"
                type="button"
                disabled={loading}
                onclick={oncreate}
            >
                <Icon name="folder-plus" size={14} />New folder
            </button>
        {/if}
    </div>
</nav>
