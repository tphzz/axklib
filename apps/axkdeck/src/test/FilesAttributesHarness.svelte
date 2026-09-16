<script lang="ts">
    import FilesInspector from '../features/files/FilesInspector.svelte';
    import { filesystemEntry } from '../lib/testing/filesystem';

    const parameters = new URLSearchParams(window.location.search);
    const width = Number(parameters.get('width') ?? 320);
    const scale = Number(parameters.get('scale') ?? 1);
    const kind = parameters.get('kind') ?? 'file';
    const directory = kind === 'directory';
    const entry = filesystemEntry({
        kind: directory ? 'directory' : 'file',
        name: directory ? 'Samples' : 'Piano Left',
        path: directory ? '/Samples' : '/Piano/SMPL/Piano Left',
        sizeBytes: directory ? null : 304664,
        objectId: directory ? null : 'sample',
        rawAttributes: kind === 'fat' ? 'FAT 0x23' : directory ? 'SFS 0xB4646972' : 'SFS 0xBE000000',
        storage: 'Record 45; 1 extent; 327680 allocated bytes',
        attributes:
            kind === 'fat'
                ? ['Read-only', 'Hidden', 'Archive'].map((label) => ({
                      code: `fat.${label.toLowerCase()}`,
                      label,
                      value: 'Yes',
                      summary: label,
                      description: `Yes: the FAT ${label.toLowerCase()} flag is set.\n\nWhen clear, this row is omitted.`,
                  }))
                : [
                      {
                          code: 'sfs.write',
                          label: directory ? 'Temporary directory write flag' : 'File write flag',
                          value: directory ? 'Disabled' : 'Enabled',
                          summary: directory ? '' : 'File write flag: Enabled',
                          description: directory
                              ? 'Enabled: directory data writes are temporarily enabled.\nDisabled: the temporary write flag is clear.\n\nDirectory updates enable this flag as needed and clear it afterward.'
                              : 'Enabled: permits file data writes and extension.\nDisabled: blocks file data writes and extension.\n\nWriting also requires a writable handle and partition. Rename, deletion and overall image editability are governed by separate checks.',
                      },
                      {
                          code: 'sfs.record-state',
                          label: 'Record state',
                          value: 'Live',
                          summary: '',
                          description:
                              'Live: the record is marked active.\nInactive: the live marker is clear.\n\nThe live marker is set when the record is created and cleared on final release.',
                      },
                      {
                          code: 'sfs.record-type',
                          label: 'Native record type',
                          value: directory ? 'Directory' : 'Ordinary',
                          summary: '',
                          description:
                              "Ordinary: holds file data.\nDirectory: contains filesystem entries.\n\nIdentifies the record's role in the SFS filesystem.",
                      },
                      {
                          code: 'sfs.allocation',
                          label: 'Allocation policy',
                          value: 'Large unit',
                          summary: '',
                          description:
                              "Standard: ordinary cluster allocation; ordinary growth uses at least two clusters.\nLarge unit: payload extents are aligned and rounded to the partition's larger allocation unit.\n\nAllocated capacity can exceed the logical file size under either policy.",
                      },
                      {
                          code: 'sfs.allocation-unit',
                          label: 'Allocation unit',
                          value: '32 clusters (32768 B)',
                          summary: '',
                          description:
                              "Size of the partition's large payload allocation unit, in clusters and equivalent bytes.\n\nA cluster is a group of disk sectors. The allocation unit specifies the increment used when allocating payload storage.",
                      },
                      {
                          code: 'sfs.references',
                          label: 'Filesystem references',
                          value: directory ? '2' : '1',
                          summary: '',
                          description:
                              "Number of filesystem references to this record.\n\nOrdinary files normally have one reference. A directory normally has two (its parent's entry and its own '.'), plus one for each child directory's '..'. The root's '.' and '..' refer to itself.",
                      },
                  ],
    });
    let navigated = $state(false);
</script>

<div class="fixture" style:width={`${width}px`} style:zoom={scale} data-navigated={navigated}>
    <FilesInspector {entry} canShowDevice={!directory} onshowdevice={() => (navigated = true)} />
</div>

<style>
    .fixture {
        display: flex;
        height: 580px;
        margin: 12px;
        margin-left: auto;
        background: var(--color-panel);
    }
    .fixture :global(.inspector) {
        width: 100%;
        min-width: 0;
    }
</style>
