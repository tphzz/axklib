<script module lang="ts">
    interface ObserverPool {
        observer: IntersectionObserver;
        subscribers: Map<Element, (visible: boolean) => void>;
    }

    const observerPools = new Map<Element | null, ObserverPool>();

    function observeNearViewport(node: Element, onchange: (visible: boolean) => void): () => void {
        const root = node.closest('.collection-body');
        let pool = observerPools.get(root);
        if (!pool) {
            const subscribers = new Map<Element, (visible: boolean) => void>();
            const observer = new IntersectionObserver(
                (entries) => {
                    for (const entry of entries) subscribers.get(entry.target)?.(entry.isIntersecting);
                },
                { root, rootMargin: '80px' },
            );
            pool = { observer, subscribers };
            observerPools.set(root, pool);
        }
        pool.subscribers.set(node, onchange);
        pool.observer.observe(node);
        return () => {
            pool.observer.unobserve(node);
            pool.subscribers.delete(node);
            if (pool.subscribers.size === 0) {
                pool.observer.disconnect();
                observerPools.delete(root);
            }
        };
    }
</script>

<script lang="ts">
    import { onMount } from 'svelte';
    import type { WaveformBin } from '../types';
    import type { WaveformTimeline } from '../waveformTimeline';
    import Waveform from './Waveform.svelte';

    interface Props {
        values: readonly WaveformBin[];
        timeline: WaveformTimeline;
        playheadRatio?: number;
        onvisible?: () => void;
    }

    let { values, timeline, playheadRatio = 0, onvisible = () => undefined }: Props = $props();
    let host: HTMLDivElement;
    let visible = $state(typeof IntersectionObserver === 'undefined');

    onMount(() => {
        if (typeof IntersectionObserver === 'undefined') {
            onvisible();
            return;
        }
        return observeNearViewport(host, (nextVisible) => {
            visible = nextVisible;
            if (visible) onvisible();
        });
    });
</script>

<div class="viewport-waveform" bind:this={host}>
    {#if visible}
        <Waveform {values} {timeline} {playheadRatio} />
    {/if}
</div>
