<script lang="ts">
    import type { InspectorRelationshipGroup } from '../types';

    interface Props {
        groups: readonly InspectorRelationshipGroup[];
        onnavigate?: (objectId: string, focusTarget: boolean) => void;
    }

    let { groups, onnavigate }: Props = $props();
</script>

<section class="inspector-relationships inspector-section" aria-labelledby="inspector-relationships-heading">
    <h4 id="inspector-relationships-heading">Relationships</h4>
    {#if groups.length === 0}
        <p class="inspector-relationships-empty">No direct relationships</p>
    {:else}
        {#each groups as group (group.objectType)}
            <div class="inspector-relationship-group">
                <h5>{group.label}</h5>
                <ul>
                    {#each group.items as item (item.id)}
                        <li>
                            {#if item.navigable && item.objectId}
                                <button
                                    type="button"
                                    onclick={(event) => onnavigate?.(item.objectId!, event.detail === 0)}
                                >
                                    <strong title={item.name}>{item.name}</strong>
                                    {#if item.detail}
                                        <span title={item.detailTitle}>{item.detail}</span>
                                    {/if}
                                </button>
                            {:else}
                                <div class="inspector-relationship-unresolved" title="Not resolvable">
                                    <strong title={item.name}>{item.name}</strong>
                                    {#if item.detail}
                                        <span title={item.detailTitle}>{item.detail}</span>
                                    {/if}
                                </div>
                            {/if}
                        </li>
                    {/each}
                </ul>
            </div>
        {/each}
    {/if}
</section>
