<script lang="ts">
    import type { AuditionState } from '../audio/auditionController';

    interface Props {
        available: boolean;
        autoplay: boolean;
        state: AuditionState;
        label: string;
        onautoplaychange: (enabled: boolean) => void;
    }

    let { available, autoplay, state, label, onautoplaychange }: Props = $props();
    const active = $derived(state.status === 'preparing' || state.status === 'playing');
    const statusText = $derived(
        state.status === 'preparing'
            ? `Preparing ${label || 'audio'}`
            : state.status === 'playing'
              ? `Playing ${label || 'audio'}`
              : state.status === 'failed'
                ? state.error || 'Playback failed'
                : 'Ready to audition',
    );
</script>

{#if available}
    <section class="audition-bar" aria-label="Audition controls" aria-live="polite">
        <label class="audition-autoplay">
            <input
                checked={autoplay}
                type="checkbox"
                onchange={(event) => onautoplaychange(event.currentTarget.checked)}
            />
            <span>Autoplay</span>
        </label>
        <span class:active class="audition-status">{statusText}</span>
    </section>
{/if}
