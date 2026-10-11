<script lang="ts">
    import type { ComponentProps } from 'svelte';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';
    import StorageBrowserPanel from './StorageBrowserPanel.svelte';

    let { title, ...browser }: ComponentProps<typeof StorageBrowserPanel> & { title: string } = $props();
</script>

<div
    class="dialog-backdrop storage-picker-backdrop"
    role="presentation"
    onclick={(event) => {
        if (event.target === event.currentTarget) browser.oncancel();
    }}
>
    <div
        class="dialog-shell dialog-shell-wide storage-picker"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        use:modal={{ onescape: browser.oncancel }}
    >
        <header class="dialog-header">
            <h2>{title}</h2>
            <button class="icon-button" type="button" aria-label="Close" onclick={browser.oncancel}>
                <Icon name="close" size={15} />
            </button>
        </header>
        <StorageBrowserPanel {...browser} />
    </div>
</div>
