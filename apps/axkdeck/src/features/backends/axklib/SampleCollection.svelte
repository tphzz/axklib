<script lang="ts">
    import { onDestroy, tick, untrack } from 'svelte';
    import ContainedObjectWorkspace from '../../../lib/components/ContainedObjectWorkspace.svelte';
    import type { ContainedObjectWorkspaceProps } from '../../../lib/components/containedObjectWorkspaceProps';
    import type { PackageExportSelectionState } from '../../../lib/objectSelection';
    import type { SampleStructureItem } from '../../../lib/types';
    import { compareNamedItems } from '../../../lib/naturalSort';
    import { objectEditors } from '../../object-editor/context';
    let {
        sessionId,
        revision,
        lowerOpen = $bindable(false),
        ...props
    }: ContainedObjectWorkspaceProps & {
        sessionId: number | null;
        revision: number;
        lowerOpen?: boolean;
    } = $props();
    const editors = objectEditors();

    function select(item: SampleStructureItem): void {
        if (props.view === 'samples') lowerOpen = true;
        props.onsampleselect(item);
    }
    function selectionChanged(selection: PackageExportSelectionState): void {
        const previousCount = props.selection?.items.length ?? 0;
        props.onselectionchange?.(selection);
        if (props.view !== 'samples') return;
        const selected = props.samples
            .filter((item) => selection.items.some((row) => row.kind === 'SBNK' && row.objectId === item.objectId))
            .toSorted(compareNamedItems);
        if (
            (selected.length > 1 || previousCount > 1) &&
            selected.length &&
            !selected.some((item) => item.objectId === props.activeSampleId)
        )
            select(selected[0]!);
    }
    async function duplicate(source: SampleStructureItem): Promise<void> {
        if (!editors || sessionId === null) return;
        const session = sessionId;
        const sameVolume = (item: SampleStructureItem) =>
            item.object.partitionIndex === source.object.partitionIndex &&
            item.object.volumeName === source.object.volumeName;
        await editors.duplication.open(
            session,
            source.objectId,
            props.samples.filter(sameVolume).map((item) => item.name),
            async (name) => {
                await tick();
                if (sessionId !== session) throw new Error('The source image is no longer open');
                const created = props.samples.find((item) => sameVolume(item) && item.name === name);
                if (!created) throw new Error('The duplicated Sample is not available in the refreshed volume');
                props.onquerychange('primary', '');
                props.onselectionchange?.({
                    items: [
                        {
                            kind: 'SBNK',
                            objectId: created.objectId,
                            name: created.name,
                            typeLabel: 'Sample',
                            partitionIndex: created.object.partitionIndex,
                            partitionName: created.object.partitionName,
                            volumeName: created.object.volumeName,
                        },
                    ],
                    anchors: {},
                });
                select(created);
            },
        );
    }
    $effect(() => {
        const session = sessionId;
        const version = revision;
        const ids =
            props.view === 'samples'
                ? (props.selection?.items ?? [])
                      .filter(
                          (row) =>
                              row.kind === 'SBNK' && props.samples.some((sample) => sample.objectId === row.objectId),
                      )
                      .map((row) => row.objectId)
                : [];
        untrack(() => {
            if (session !== null) void editors?.comparison.select(session, version, ids);
            else editors?.comparison.clear();
        });
    });
    onDestroy(() => editors?.comparison.clear());
</script>

<ContainedObjectWorkspace
    {...props}
    onsampleselect={select}
    onselectionchange={selectionChanged}
    onduplicatesample={props.view === 'samples' && props.objectRenameAvailable && editors ? duplicate : undefined}
    onconvertsample={editors && sessionId !== null
        ? (sample) => {
              void editors.openConversion(sessionId!, sample.objectId);
          }
        : undefined}
/>
