<script lang="ts">
    import { untrack } from 'svelte';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import type { EditorValues } from '../../../object-editor/draft.svelte';
    import { objectEditors } from '../../../object-editor/context';
    import KeyboardMapping from '../../../object-editor/KeyboardMapping.svelte';
    import MappingOpen from '../../../program-mapping/MappingOpen.svelte';
    import { BankDraft } from '../bank/draft.svelte';
    import { sampleMappingRange, sampleMappingPatch, sampleMappingAxes } from './mapping';
    import { noteName } from './geometry';
    import { aSeriesMapping } from '../mapping';
    import { loadMappingMembers } from '../bank/mappingLoad';
    import { userFacingMessage } from '../../../../lib/userFacingMessage';
    import { mappingKeyboard } from '../../../program-mapping/keyboardAudition.svelte';
    let { document, disabled }: { document: ObjectEditorDocument; disabled: boolean } = $props();
    const editors = objectEditors();
    let gestureValues: EditorValues | undefined;
    const bank = $derived(document.draft instanceof BankDraft);
    const keyboard = mappingKeyboard(
        () => document,
        () => (bank ? 'bank' : 'sample'),
    );
    const bankModel = $derived(bank && editors ? aSeriesMapping('members', document, editors) : null);
    const values = $derived(document.draft.values);
    const limits = $derived(bank ? (bankModel?.data.limits ?? null) : sampleMappingRange(values));
    const editable = $derived(!disabled && !bank && sampleMappingAxes(document).keys);
    const name = $derived(
        bank
            ? (bankModel?.data.selections.find((member) => member.value === bankModel.data.selectionId)?.label ?? '')
            : document.detail!.object.name,
    );
    const zones = $derived(
        bankModel?.data.zones ??
            (limits
                ? [
                      {
                          ...limits,
                          id: 'sample',
                          label: name,
                          selected: true,
                          root: Number(values.root_key),
                      },
                  ]
                : []),
    );
    let loading = $state(false);
    let loadError = $state('');
    $effect(() => {
        if (!bank || !editors || !document.detail) return;
        const owner = document,
            detail = document.detail;
        let current = true;
        loading = true;
        loadError = '';
        void untrack(() => loadMappingMembers(editors, owner, () => current && owner.detail === detail))
            .catch((error) => {
                if (current) loadError = userFacingMessage(error);
            })
            .finally(() => {
                if (current) loading = false;
            });
        return () => {
            current = false;
        };
    });
</script>

{#snippet tools()}
    {#if bank}
        <MappingOpen role="bank" {document} label="Bank Mapping" />
        <MappingOpen role="members" {document} label="Member Mapping" />
        {#if name}<span class="editor-meta">{name}</span>{/if}
        {#if loading || loadError}<span class="editor-meta" role="status">{loadError || 'Loading mappings'}</span>{/if}
    {:else}<MappingOpen role="sample" {document} />{/if}
{/snippet}
{#if limits}
    <KeyboardMapping
        {limits}
        velocity={keyboard.state.velocity}
        onvelocity={(value) => {
            keyboard.release();
            keyboard.state.velocity = value;
        }}
        onpress={keyboard.press}
        onrelease={keyboard.release}
        formatNote={noteName}
        {tools}
        rangeLabel="Sample range"
        {zones}
        editableAxes={{ keys: editable, velocity: false }}
        disabled={!editable}
        onselect={(id) => {
            const zone = zones.find((zone) => zone.id === id);
            if (zone?.selectionId !== undefined) bankModel?.select(zone.selectionId);
        }}
        onchange={(range, boundaries) => {
            if (editable) document.draft.patch(sampleMappingPatch(range, boundaries, gestureValues));
        }}
        onbegin={() => {
            if (!bank) {
                gestureValues = { ...document.draft.storedValues };
                document.draft.beginGesture();
            }
        }}
        onend={(cancelled) => {
            if (!bank) {
                if (cancelled) document.draft.cancelGesture();
                else document.draft.endGesture();
            }
            gestureValues = undefined;
        }}
    />
{:else}<div class="mapping-tools">{@render tools()}</div>{/if}

<style>
    .mapping-tools {
        display: flex;
        gap: 5px;
        flex-wrap: wrap;
        margin-top: 8px;
    }
</style>
