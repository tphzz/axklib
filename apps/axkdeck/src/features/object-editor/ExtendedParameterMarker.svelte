<script lang="ts">
    import Icon from '../../lib/components/Icon.svelte';
    let { decorative = false, a5000Only = false }: { decorative?: boolean; a5000Only?: boolean } = $props();
</script>

<span
    class="extended-parameter"
    title={decorative
        ? undefined
        : a5000Only
          ? 'Available on A5000 only'
          : 'Additional settings are available in a4k/a5k format'}
    aria-label={decorative ? undefined : a5000Only ? 'A5000 only' : 'A4000/A5000 extension'}
    aria-hidden={decorative || undefined}><Icon name="plus" size={10} /></span
>
