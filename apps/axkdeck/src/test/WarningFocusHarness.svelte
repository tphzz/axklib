<script lang="ts">
    import { onMount } from 'svelte';
    import ExperimentalWarningDialog from '../lib/components/ExperimentalWarningDialog.svelte';

    let trigger: HTMLButtonElement;
    let open = $state(false);
    let acknowledgements = $state(0);
    onMount(() => {
        trigger.focus();
        open = true;
    });
</script>

<button class="secondary-button" bind:this={trigger} onclick={() => (open = true)}>Show warning</button>
<output aria-label="Acknowledgements">{acknowledgements}</output>
{#if open}
    <ExperimentalWarningDialog
        onacknowledge={() => {
            acknowledgements += 1;
            open = false;
        }}
    />
{/if}
