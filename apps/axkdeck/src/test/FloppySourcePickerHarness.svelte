<script lang="ts">
    import ServerStoragePicker from '../lib/components/ServerStoragePicker.svelte';
    import type { ImageTransport } from '../lib/transport';
    import type { DirectoryRef } from '../lib/storageLocations';

    let selection = $state('');
    let visible = $state(true);
    const transport = {
        sandboxRoots: async () => [{ id: 'test', displayName: 'Test disks', writable: false }],
        sandboxDirectory: async (directory: DirectoryRef) => ({
            directory,
            entries: directory.relativePath
                ? [1, 2, 3].map((index) => ({
                      name: `disk${index}`,
                      relativePath: `norddrms/disk${index}`,
                      kind: 'DIRECTORY',
                      size: null,
                  }))
                : [
                      { name: 'norddrms', relativePath: 'norddrms', kind: 'DIRECTORY', size: null },
                      ...Array.from({ length: 40 }, (_, index) => ({
                          name: `disk${index + 1}.img`,
                          relativePath: `disk${index + 1}.img`,
                          kind: 'FILE',
                          size: 1474560,
                      })),
                  ],
            truncated: false,
            nextCursor: null,
        }),
    } as unknown as ImageTransport;
</script>

<output>{selection}</output>
{#if visible}
    <ServerStoragePicker
        {transport}
        mode="floppy-source"
        title="Choose floppy source"
        multiple={true}
        extensions={['img', 'ima']}
        initialDirectory={{ rootId: 'test', relativePath: '' }}
        onselect={(value) => {
            selection = value.kind + ':' + value.reference.relativePath;
            visible = false;
        }}
        onselectmany={(values) => {
            selection = values.map((value) => value.reference.relativePath).join(',');
            visible = false;
        }}
        oncancel={() => {
            visible = false;
            selection = 'cancelled';
        }}
    />
{/if}
