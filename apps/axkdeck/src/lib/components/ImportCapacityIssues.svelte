<script lang="ts">
    import type { importCapacityGroups } from '../importCapacity';
    let { groups }: { groups: ReturnType<typeof importCapacityGroups> } = $props();
</script>

{#if groups.length}
    <section class="package-conflicts" role="alert" aria-label="Partition space issues">
        <strong>{groups.length} {groups.length === 1 ? 'issue prevents' : 'issues prevent'} import</strong>
        {#each groups as group (group.partitionIndex)}
            <section class="capacity-issue">
                <strong>Not enough space on {group.name}</strong>
                <p>Choose another partition or free space, then review again.</p>
                <details>
                    <summary>Technical details</summary>
                    <div class="capacity-details">
                        {#each group.conflicts as conflict}<p>{conflict.message}</p>{/each}
                    </div>
                </details>
            </section>
        {/each}
    </section>
{/if}

<style>
    .capacity-issue {
        margin-top: 8px;
    }
    summary {
        cursor: pointer;
    }
    .capacity-details {
        overflow-wrap: anywhere;
    }
</style>
