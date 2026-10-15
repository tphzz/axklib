<script lang="ts">
    import type { Snippet } from 'svelte';
    let {
        heading,
        label = 'Object inspector',
        actions,
        children,
        footer,
    }: { heading: string; label?: string; actions?: Snippet; children: Snippet; footer?: Snippet } = $props();
</script>

<aside class="inspector" aria-label={label}>
    <div class="panel-heading">
        <div>
            <p class="eyebrow">Inspector</p>
            <h2>{heading}</h2>
        </div>
        {@render actions?.()}
    </div>
    <div class="inspector-body"><div class="inspector-content">{@render children()}</div></div>
    {@render footer?.()}
</aside>
