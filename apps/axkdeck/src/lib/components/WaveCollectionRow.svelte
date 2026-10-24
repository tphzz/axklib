<script lang="ts">
    import { onDestroy } from 'svelte';
    import type { WaveformBin } from '../types';
    import { waveformPlayheadRatio, type WaveformTimeline } from '../waveformTimeline';
    import Icon from './Icon.svelte';
    import ViewportWaveform from './ViewportWaveform.svelte';
    import type { WaveformReadDraw } from '../readDiagnostics';

    let {
        objectId,
        name,
        index,
        metadata,
        format,
        values,
        timeline,
        active = false,
        selected = false,
        playing = false,
        preparing = false,
        playheadFrame = 0,
        unavailable = '',
        onselect,
        onseek,
        onpreview,
        ondraw,
        onplay,
        onstop,
        onprefetch = () => undefined,
        oncontextmenu,
        onkeydown,
    }: {
        objectId: string;
        name: string;
        index: number;
        metadata: string;
        format: string;
        values: readonly WaveformBin[];
        timeline: WaveformTimeline;
        active?: boolean;
        selected?: boolean;
        playing?: boolean;
        preparing?: boolean;
        playheadFrame?: number;
        unavailable?: string;
        onselect: (event: MouseEvent) => void;
        onseek: (event: MouseEvent) => void;
        onpreview: (bins: number) => void;
        ondraw?: (draw: WaveformReadDraw) => void;
        onplay: () => void;
        onstop: () => void;
        onprefetch?: () => void;
        oncontextmenu?: (event: MouseEvent) => void;
        onkeydown: (event: KeyboardEvent) => void;
    } = $props();
    let timer: ReturnType<typeof setTimeout> | undefined;
    function clearPrefetch() {
        if (timer !== undefined) clearTimeout(timer);
        timer = undefined;
    }
    function prefetch() {
        clearPrefetch();
        if (!unavailable) timer = setTimeout(onprefetch, 150);
    }
    onDestroy(clearPrefetch);
</script>

<!-- The selection overlay covers blank cells while independent controls remain above it. -->
<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div class="wave-data-row" class:active class:selected role="group" aria-label={`${name} Wave Data`} {oncontextmenu}>
    <button
        class="wave-data-selection"
        data-collection-object-id={objectId}
        data-navigation-index={index}
        type="button"
        aria-label={`Inspect ${name}`}
        title={`${name}\n${metadata}\n${format}`}
        aria-pressed={selected}
        onclick={onselect}
        {onkeydown}
    ></button>
    <strong class="wave-data-identity">{name}</strong>
    <span class="wave-data-meta">{metadata}</span>
    <button class="waveform-seek" type="button" aria-label={`Seek ${name}`} onclick={onseek} disabled={!!unavailable}>
        <ViewportWaveform
            {values}
            {timeline}
            onvisible={onpreview}
            {ondraw}
            redrawKey={active}
            playheadRatio={playing ? waveformPlayheadRatio(timeline, playheadFrame, timeline.sampleRate) : 0}
        />
    </button>
    <span class="wave-data-format">{format}</span>
    <button
        class="wave-data-playback icon-button"
        type="button"
        aria-label={playing || preparing ? `Stop ${name}` : `Play ${name}`}
        title={unavailable || (preparing ? 'Stop preparing audio' : playing ? 'Stop' : 'Play')}
        disabled={!!unavailable}
        onpointerenter={prefetch}
        onpointerleave={clearPrefetch}
        onfocus={prefetch}
        onblur={clearPrefetch}
        onclick={(event) => {
            event.stopPropagation();
            clearPrefetch();
            if (playing || preparing) onstop();
            else onplay();
        }}
    >
        <Icon name={playing || preparing ? 'stop' : 'play'} size={13} />
    </button>
</div>
