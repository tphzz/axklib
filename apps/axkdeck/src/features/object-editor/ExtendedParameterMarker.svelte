<script lang="ts">
    import Icon from '../../lib/components/Icon.svelte';
    let { decorative = false }: { decorative?: boolean } = $props();
</script>

<span
    class="extended-parameter"
    title={decorative ? undefined : 'Additional settings are available in a4k/a5k format'}
    aria-label={decorative ? undefined : 'A4000/A5000 extension'}
    aria-hidden={decorative || undefined}><Icon name="plus" size={10} /></span
>
