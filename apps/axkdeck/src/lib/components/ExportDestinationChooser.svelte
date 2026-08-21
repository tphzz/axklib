<script lang="ts">
    import Icon from './Icon.svelte';

    interface Props {
        desktop: boolean;
        disabled?: boolean;
        onworkspace: () => void;
        onlocal: () => void;
    }

    let { desktop, disabled = false, onworkspace, onlocal }: Props = $props();
</script>

<div class="export-destination-choices">
    <button class="source-choice-button" type="button" {disabled} onclick={onworkspace}>
        <Icon name="folder-open" size={18} />
        <span><strong>Storage location</strong><small>Save to a configured workspace</small></span>
    </button>
    {#if desktop}
        <button class="source-choice-button" type="button" {disabled} onclick={onlocal}>
            <Icon name="hard-drive" size={18} />
            <span><strong>This computer</strong><small>Save with the desktop file chooser</small></span>
        </button>
    {/if}
</div>
