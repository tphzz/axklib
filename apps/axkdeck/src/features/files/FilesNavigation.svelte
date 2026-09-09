<script lang="ts">
    import Icon from '../../lib/components/Icon.svelte';
    import { linearNavigationIndex } from '../../lib/collectionNavigation';
    import type { FilesController } from './controller.svelte';
    import type { FilesystemEntry } from '../../lib/filesystem';
    import ObjectContextMenu from '../../lib/components/ObjectContextMenu.svelte';
    let {
        controller,
        onexport,
        onimport,
        exportBlocked = false,
        additionalImports = () => [],
    }: {
        controller: FilesController;
        onexport?: (entries: FilesystemEntry[]) => void;
        onimport?: (root: FilesystemEntry) => void;
        exportBlocked?: boolean;
        additionalImports?: (root: FilesystemEntry) => { label: string; action: () => void }[];
    } = $props();
    let menu = $state<{ root: FilesystemEntry; left: number; top: number } | null>(null);
    const canImport = (root: FilesystemEntry): boolean =>
        !!onimport &&
        !!controller.capabilitiesFor(root.id)?.putFile &&
        !!controller.capabilitiesFor(root.id)?.createDirectory;
    function openMenu(event: MouseEvent | KeyboardEvent, root: FilesystemEntry): void {
        if ((!onexport && !canImport(root)) || exportBlocked || root.filesystemMetadata || root.issue) return;
        event.preventDefault();
        const rect = (event.currentTarget as HTMLElement).getBoundingClientRect();
        menu = {
            root,
            left: event instanceof MouseEvent ? event.clientX : rect.left,
            top: event instanceof MouseEvent ? event.clientY : rect.bottom,
        };
    }
    function navigate(event: KeyboardEvent, index: number): void {
        const next = linearNavigationIndex(event.key, index, controller.roots.length);
        if (next === null) return;
        event.preventDefault();
        const root = controller.roots[next];
        void controller.chooseRoot(root.id);
        (event.currentTarget as HTMLElement).parentElement
            ?.querySelectorAll<HTMLButtonElement>('button')
            [next]?.focus();
    }
</script>

<section class="image-contents" aria-label="Filesystem navigation" data-workspace-background>
    <div class="panel-heading">
        <div>
            <p class="eyebrow">Contents</p>
            <h2>Filesystem</h2>
        </div>
    </div>
    <div class="filesystem-roots" role="group" aria-label="Filesystem roots" data-workspace-background>
        {#each controller.roots as root, index (root.id)}
            <button
                type="button"
                class:selected={controller.rootId === root.id}
                aria-pressed={controller.rootId === root.id}
                tabindex={controller.rootId === root.id ? 0 : -1}
                title={root.name}
                onclick={() => void controller.chooseRoot(root.id)}
                oncontextmenu={(event) => openMenu(event, root)}
                onkeydown={(event) =>
                    event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10')
                        ? openMenu(event, root)
                        : navigate(event, index)}
            >
                <Icon name="hard-drive" size={15} /><span>{root.name}</span>
            </button>
        {/each}
    </div>
</section>
{#if menu}
    <ObjectContextMenu
        objectName={menu.root.name}
        left={menu.left}
        top={menu.top}
        onimportdirectory={canImport(menu.root) ? () => onimport?.(menu!.root) : undefined}
        additionalImports={additionalImports(menu.root)}
        onexportfiles={onexport ? () => onexport?.([menu!.root]) : undefined}
        onclose={() => (menu = null)}
    />
{/if}

<style>
    .filesystem-roots {
        overflow-y: auto;
        min-height: 0;
        padding: 0 calc(8px + var(--overlay-scrollbar-clearance)) 8px 8px;
    }
    button {
        width: 100%;
        display: flex;
        align-items: center;
        gap: 7px;
        min-height: 26px;
        padding: 3px 6px;
        border: 1px solid transparent;
        border-radius: 4px;
        background: transparent;
        text-align: left;
        color: var(--color-text);
        font-size: 11px;
    }
    button.selected {
        background: var(--color-selection);
        border-color: #497590;
    }
    button span {
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
</style>
