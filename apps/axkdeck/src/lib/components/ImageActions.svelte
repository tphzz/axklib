<script lang="ts">
    import Icon from './Icon.svelte';
    import { useDevices } from '../../features/physical-devices/workflow.svelte';
    const devices = useDevices();
    interface Props {
        image: { displayName: string } | null;
        sourceNames?: string[];
        opening: boolean;
        storageLocationsAvailable: boolean;
        onopen: () => void;
        oncreate: () => void;
        onclose: () => void;
        onintegrity?: () => void;
        onmanagelocations: () => void;
    }
    let {
        image,
        sourceNames = [],
        opening,
        storageLocationsAvailable,
        onopen,
        oncreate,
        onclose,
        onintegrity = () => undefined,
        onmanagelocations,
    }: Props = $props();
    let imageMenuOpen = $state(false);
    const pathParts = $derived(image?.displayName.replaceAll('\\', '/').split('/').filter(Boolean) ?? []);
    const multiple = $derived(sourceNames.length > 1);
    const imageName = $derived(multiple ? 'Multiple' : (pathParts.at(-1) ?? 'No image open'));
    const imageLocation = $derived(multiple ? `${sourceNames.length} sources` : pathParts.slice(0, -1).join('/'));
    const imageTitle = $derived(multiple ? sourceNames.join('\n') : image?.displayName);
</script>

<svelte:window
    onclick={() => (imageMenuOpen = false)}
    onkeydown={(event) => {
        if (event.key === 'Escape') imageMenuOpen = false;
    }}
/>
<section class="image-summary" aria-label="Open images">
    <header class="image-summary-heading">
        <p class="eyebrow">Image</p>
        {#if storageLocationsAvailable}
            <button
                class="icon-button"
                type="button"
                aria-label="Image options"
                aria-expanded={imageMenuOpen}
                title="Image options"
                disabled={opening}
                onclick={(event) => {
                    event.stopPropagation();
                    imageMenuOpen = !imageMenuOpen;
                }}
            >
                <Icon name="more" size={15} />
            </button>
        {/if}
    </header>

    {#if image}
        <div
            class="active-image"
            class:multiple
            role="group"
            aria-label={multiple ? `${sourceNames.length} open sources` : `Current image: ${imageName}`}
            aria-busy={opening}
        >
            <Icon name="hard-drive" size={16} />
            <span class="active-image-copy">
                <strong title={imageTitle}>{imageName}</strong>
                <small title={imageLocation}>{opening ? 'Opening image' : imageLocation || 'Storage location'}</small>
            </span>
            <div class="active-image-actions">
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Open"
                    title="Open"
                    disabled={opening}
                    onclick={onopen}
                >
                    <Icon name="folder-open" size={15} />
                </button>
                <button
                    class="icon-button"
                    type="button"
                    aria-label={multiple ? 'Eject all images' : 'Eject image'}
                    title={multiple ? 'Eject all images' : 'Eject image'}
                    disabled={opening}
                    onclick={onclose}
                >
                    <Icon name="eject" size={15} />
                </button>
            </div>
        </div>
    {:else}
        <div class="image-empty-state">
            <div class="image-empty-actions">
                <button class="primary-button" type="button" disabled={opening} onclick={onopen}>
                    <Icon name="folder-open" size={14} /> Open
                </button>
                <button class="secondary-button" type="button" disabled={opening} onclick={oncreate}>
                    <Icon name="file-plus" size={14} /> Create image
                </button>
            </div>
        </div>
    {/if}

    {#if imageMenuOpen}
        <div
            class="image-options-menu"
            role="menu"
            tabindex="-1"
            onclick={(event) => event.stopPropagation()}
            onkeydown={(event) => event.stopPropagation()}
        >
            {#if devices}
                <button
                    type="button"
                    role="menuitem"
                    onclick={() => {
                        imageMenuOpen = false;
                        void devices.show();
                    }}><Icon name="refresh" size={14} /> Device operation status...</button
                >
            {/if}
            {#if image}
                <button
                    type="button"
                    role="menuitem"
                    onclick={() => {
                        imageMenuOpen = false;
                        onintegrity();
                    }}
                >
                    <Icon name="info" size={14} /> Disk integrity...
                </button>
                <button
                    type="button"
                    role="menuitem"
                    onclick={() => {
                        imageMenuOpen = false;
                        oncreate();
                    }}
                >
                    <Icon name="file-plus" size={14} /> Create new image
                </button>
            {/if}
            <button
                type="button"
                role="menuitem"
                onclick={() => {
                    imageMenuOpen = false;
                    onmanagelocations();
                }}
            >
                <Icon name="settings" size={14} /> Storage locations
            </button>
        </div>
    {/if}
</section>

<style>
    @media (max-width: 540px) {
        .active-image.multiple {
            grid-template-columns: 18px minmax(0, 1fr);
        }
        .multiple .active-image-actions {
            grid-column: 1 / -1;
            justify-self: end;
        }
    }
</style>
