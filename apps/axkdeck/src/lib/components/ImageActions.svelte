<script lang="ts">
    import Icon from './Icon.svelte';
    interface Props {
        image: { displayName: string } | null;
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
    const imageName = $derived(pathParts.at(-1) ?? 'No image open');
    const imageLocation = $derived(pathParts.slice(0, -1).join('/'));
</script>

<svelte:window
    onclick={() => (imageMenuOpen = false)}
    onkeydown={(event) => {
        if (event.key === 'Escape') imageMenuOpen = false;
    }}
/>
<section class="image-summary" aria-label="Active image">
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
        <div class="active-image" role="group" aria-label={`Current image: ${imageName}`} aria-busy={opening}>
            <Icon name="hard-drive" size={16} />
            <span class="active-image-copy">
                <strong title={image.displayName}>{imageName}</strong>
                <small title={imageLocation}>{opening ? 'Opening image' : imageLocation || 'Storage location'}</small>
            </span>
            <div class="active-image-actions">
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Open another image"
                    title="Open another image"
                    disabled={opening}
                    onclick={onopen}
                >
                    <Icon name="folder-open" size={15} />
                </button>
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Eject image"
                    title="Eject image"
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
                    <Icon name="folder-open" size={14} /> Open image
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
            {#if image}
                <button
                    type="button"
                    role="menuitem"
                    onclick={() => {
                        imageMenuOpen = false;
                        onintegrity();
                    }}
                >
                    <Icon name="info" size={14} /> Image integrity...
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
