<script lang="ts">
    type IconName =
        | 'archive'
        | 'chevron'
        | 'check'
        | 'close'
        | 'disc'
        | 'eject'
        | 'folder'
        | 'folder-plus'
        | 'grid'
        | 'hard-drive'
        | 'layers'
        | 'list'
        | 'lock'
        | 'music'
        | 'more'
        | 'panel-left'
        | 'panel-bottom'
        | 'panel-right'
        | 'play'
        | 'refresh'
        | 'rename'
        | 'save'
        | 'search'
        | 'sliders'
        | 'server'
        | 'sparkles'
        | 'stop'
        | 'trash'
        | 'upload'
        | 'waveform';

    interface Props {
        name: IconName;
        size?: number;
        strokeWidth?: number;
        class?: string;
    }

    let { name, size = 18, strokeWidth = 1.7, class: className = '' }: Props = $props();
</script>

<svg
    class={className}
    width={size}
    height={size}
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width={strokeWidth}
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
>
    {#if name === 'search'}
        <circle cx="11" cy="11" r="7" /><path d="m20 20-4-4" />
    {:else if name === 'refresh'}
        <path d="M20 6v5h-5M4 18v-5h5" /><path d="M18.5 9A7 7 0 0 0 6 6.5L4 9m2 6a7 7 0 0 0 12.5 2.5L20 15" />
    {:else if name === 'disc'}
        <circle cx="12" cy="12" r="9" /><circle cx="12" cy="12" r="2" /><path d="M12 3a9 9 0 0 1 9 9" />
    {:else if name === 'eject'}
        <path d="m5 14 7-9 7 9H5Z" /><path d="M5 19h14" />
    {:else if name === 'folder'}
        <path d="M3 7.5V19h18V8H11l-2-3H3v2.5Z" />
    {:else if name === 'folder-plus'}
        <path d="M3 7.5V19h18V8H11l-2-3H3v2.5Z" /><path d="M12 11v5M9.5 13.5h5" />
    {:else if name === 'hard-drive'}
        <rect x="3" y="5" width="18" height="14" rx="2" /><path d="M3 13h18" /><circle cx="17" cy="16" r="1" />
    {:else if name === 'chevron'}
        <path d="m9 18 6-6-6-6" />
    {:else if name === 'music'}
        <path d="M9 18V5l10-2v13" /><circle cx="6" cy="18" r="3" /><circle cx="16" cy="16" r="3" />
    {:else if name === 'more'}
        <circle cx="5" cy="12" r="1" /><circle cx="12" cy="12" r="1" /><circle cx="19" cy="12" r="1" />
    {:else if name === 'rename'}
        <path d="M4 19h5l10-10-5-5L4 14v5Z" /><path d="m12 6 5 5" />
    {:else if name === 'trash'}
        <path d="M4 7h16M9 7V4h6v3M6 7l1 14h10l1-14M10 11v6M14 11v6" />
    {:else if name === 'grid'}
        <rect x="3" y="3" width="7" height="7" rx="1" /><rect x="14" y="3" width="7" height="7" rx="1" /><rect
            x="3"
            y="14"
            width="7"
            height="7"
            rx="1"
        /><rect x="14" y="14" width="7" height="7" rx="1" />
    {:else if name === 'list'}
        <path d="M8 6h13M8 12h13M8 18h13" /><path d="M3 6h.01M3 12h.01M3 18h.01" />
    {:else if name === 'layers'}
        <path d="m12 3-9 5 9 5 9-5-9-5Z" /><path d="m3 12 9 5 9-5M3 16l9 5 9-5" />
    {:else if name === 'sparkles'}
        <path
            d="m12 3 1.2 3.8L17 8l-3.8 1.2L12 13l-1.2-3.8L7 8l3.8-1.2L12 3ZM19 14l.7 2.3L22 17l-2.3.7L19 20l-.7-2.3L16 17l2.3-.7L19 14Z"
        />
    {:else if name === 'waveform'}
        <path d="M3 12h2l2-7 3 14 3-11 2 8 2-4h4" />
    {:else if name === 'sliders'}
        <path d="M4 6h6M14 6h6M4 12h2M10 12h10M4 18h9M17 18h3" /><circle cx="12" cy="6" r="2" /><circle
            cx="8"
            cy="12"
            r="2"
        /><circle cx="15" cy="18" r="2" />
    {:else if name === 'play'}
        <path d="m8 5 11 7-11 7V5Z" />
    {:else if name === 'stop'}
        <rect x="6" y="6" width="12" height="12" rx="1" />
    {:else if name === 'panel-left'}
        <rect x="3" y="4" width="18" height="16" rx="2" /><path d="M9 4v16" />
    {:else if name === 'panel-right'}
        <rect x="3" y="4" width="18" height="16" rx="2" /><path d="M15 4v16" />
    {:else if name === 'panel-bottom'}
        <rect x="3" y="4" width="18" height="16" rx="2" /><path d="M3 14h18" />
    {:else if name === 'archive'}
        <path d="M4 7h16v13H4V7ZM3 3h18v4H3V3Z" /><path d="M9 11h6" />
    {:else if name === 'save'}
        <path d="M5 3h12l2 2v16H5V3Z" /><path d="M8 3v6h8V3M8 21v-7h8v7" />
    {:else if name === 'upload'}
        <path d="M12 16V4M7 9l5-5 5 5M4 20h16" />
    {:else if name === 'close'}
        <path d="m6 6 12 12M18 6 6 18" />
    {:else if name === 'check'}
        <path d="m5 12 4 4L19 6" />
    {:else if name === 'lock'}
        <rect x="5" y="10" width="14" height="11" rx="2" /><path d="M8 10V7a4 4 0 0 1 8 0v3" />
    {:else if name === 'server'}
        <rect x="3" y="4" width="18" height="6" rx="2" /><rect x="3" y="14" width="18" height="6" rx="2" /><path
            d="M7 7h.01M7 17h.01M11 7h6M11 17h6"
        />
    {/if}
</svg>
