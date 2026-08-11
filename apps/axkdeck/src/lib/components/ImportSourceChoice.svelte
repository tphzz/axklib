<script lang="ts">
    import Icon from './Icon.svelte';

    interface Props {
        label: string;
        heading: string;
        description: string;
        workspaceDetail: string;
        computerDetail: string;
        computerAvailable: boolean;
        onchooseworkspace: () => void;
        onchooselocal: () => void;
    }

    let {
        label,
        heading,
        description,
        workspaceDetail,
        computerDetail,
        computerAvailable,
        onchooseworkspace,
        onchooselocal,
    }: Props = $props();
</script>

<section class="import-source-choice" aria-label={label}>
    <h3>{heading}</h3>
    <p>{description}</p>
    <button class="source-choice-button" type="button" onclick={onchooseworkspace}>
        <Icon name="folder-open" size={18} />
        <span><strong>Storage location</strong><small>{workspaceDetail}</small></span>
    </button>
    {#if computerAvailable}
        <button class="source-choice-button" type="button" onclick={onchooselocal}>
            <Icon name="hard-drive" size={18} />
            <span><strong>This computer</strong><small>{computerDetail}</small></span>
        </button>
    {/if}
</section>
