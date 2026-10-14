<script lang="ts">
    import InspectorModeFooter from '../../lib/components/InspectorModeFooter.svelte';
    import InspectorSection from '../../lib/components/InspectorSection.svelte';
    import { provideInspectorPanels } from '../../lib/inspectorPanels.svelte';
    import AttributeHelp from '../../lib/components/AttributeHelp.svelte';
    import type { FilesystemEntry, FilesystemBinding } from '../../lib/filesystem';
    import type { OpenedImage } from '../../lib/transport';
    import { nativeBindingTargets } from './nativeBindingTargets';
    let {
        entry,
        documents = [],
        diagnostics = [],
        interpretationLabel = '',
        onshowdevice = () => undefined,
    }: {
        entry: FilesystemEntry | null;
        documents?: OpenedImage['documents'];
        diagnostics?: OpenedImage['sources'][number]['diagnostics'];
        interpretationLabel?: string;
        onshowdevice?: (binding: FilesystemBinding) => void;
    } = $props();
    const nativeTargets = $derived(nativeBindingTargets(entry?.bindings ?? [], documents));
    provideInspectorPanels();
</script>

<aside class="inspector" aria-label="Filesystem inspector" data-workspace-background>
    <div class="panel-heading">
        <div>
            <p class="eyebrow">Inspector</p>
            <h2>Entry details</h2>
        </div>
    </div>
    <div class="inspector-body" data-workspace-background>
        {#if entry}
            <div class="inspector-content" data-workspace-background>
                <div class="inspector-title">
                    <span
                        >{{ root: 'Root', partition: 'Partition', directory: 'Directory', file: 'File' }[
                            entry.kind
                        ]}</span
                    >
                    <h3>{entry.name}</h3>
                </div>
                <InspectorSection
                    scope="files"
                    sectionId="properties"
                    title="Properties"
                    regionLabel="Entry properties"
                >
                    <dl class="metadata-list">
                        {#if entry.filesystemMetadata}<div>
                                <dt>Classification</dt>
                                <dd>Filesystem metadata</dd>
                            </div>{/if}
                        <div>
                            <dt>Location</dt>
                            <dd class="file-path">{entry.path || '/'}</dd>
                        </div>
                        {#if entry.sizeBytes !== null}<div>
                                <dt>File size</dt>
                                <dd>{entry.sizeBytes.toLocaleString()} B</dd>
                            </div>{/if}
                        {#if entry.kind !== 'file'}<div>
                                <dt>Entries</dt>
                                <dd>{entry.childCount.toLocaleString()}</dd>
                            </div>{/if}
                        {#if entry.interpretation}<div>
                                <dt>Interpretation</dt>
                                <dd>{interpretationLabel || entry.interpretation}</dd>
                            </div>{/if}
                        {#each entry.attributes.filter((attribute) => attribute.summary) as attribute (attribute.code)}
                            <div>
                                <dt>
                                    <AttributeHelp
                                        label={attribute.label}
                                        description={attribute.description}
                                        contextKey={entry.id}
                                    />
                                </dt>
                                <dd class="file-path">{attribute.value}</dd>
                            </div>
                        {/each}
                    </dl>
                </InspectorSection>
                {#if entry.issue}<p class="entry-issue" role="status">{entry.issue}</p>{/if}
                {#each diagnostics as issue}<p class="entry-issue" role="status">
                        {issue.code}: {issue.message}
                    </p>{/each}
                {#if entry.storage || entry.rawAttributes || entry.attributes.length}<InspectorSection
                        scope="files"
                        sectionId="storage"
                        title="Storage details"
                    >
                        <dl class="metadata-list">
                            {#if entry.rawAttributes}
                                <div>
                                    <dt>
                                        <AttributeHelp
                                            label="Native attributes"
                                            contextKey={entry.id}
                                            description="Filesystem-specific technical storage information. The hexadecimal value contains the stored attribute and type encoding."
                                        />
                                    </dt>
                                    <dd class="file-path">{entry.rawAttributes}</dd>
                                </div>
                            {/if}
                            {#each entry.attributes.filter((attribute) => !attribute.summary) as attribute (attribute.code)}
                                <div>
                                    <dt>
                                        <AttributeHelp
                                            label={attribute.label}
                                            description={attribute.description}
                                            contextKey={entry.id}
                                        />
                                    </dt>
                                    <dd class="file-path">{attribute.value}</dd>
                                </div>
                            {/each}
                        </dl>
                        {#if entry.storage}<p class="storage-details">{entry.storage}</p>{/if}
                    </InspectorSection>{/if}
            </div>
        {:else}<p class="empty-copy">No entry selected</p>{/if}
    </div>
    {#if nativeTargets.length}
        <div class="native-targets">
            {#each nativeTargets as target (target.binding.documentId)}
                <InspectorModeFooter mode="device" label={target.label} onclick={() => onshowdevice(target.binding)} />
            {/each}
        </div>
    {/if}
</aside>

<style>
    .native-targets {
        flex: none;
        max-height: 144px;
        overflow: auto;
    }
    .file-path {
        overflow-wrap: anywhere;
        white-space: normal;
    }
    .storage-details {
        font-size: 10px;
        color: var(--color-text-muted);
        overflow-wrap: anywhere;
        margin-top: 6px;
    }
    .entry-issue {
        color: #e0b765;
        font-size: 10px;
    }
</style>
