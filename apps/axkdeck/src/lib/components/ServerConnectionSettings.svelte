<script lang="ts">
    import Icon from './Icon.svelte';
    import type { RemoteServerSettingsInput, RemoteServerSettingsView } from '../serverSettings';
    import { userFacingMessage } from '../userFacingMessage';
    import { modal } from '../modal';

    interface Props {
        settings: RemoteServerSettingsView;
        onsave: (input: RemoteServerSettingsInput) => Promise<void>;
        onuselocal: () => Promise<void>;
        oncancel: () => void;
    }

    let { settings, onsave, onuselocal, oncancel }: Props = $props();
    let baseUrl = $state('');
    let bearerToken = $state('');
    let error = $state('');
    let busy = $state(false);
    let initialized = false;

    $effect(() => {
        if (initialized) return;
        baseUrl = settings.baseUrl ?? '';
        error = settings.secureStorageError ? userFacingMessage(settings.secureStorageError) : '';
        initialized = true;
    });

    async function submit(event: SubmitEvent): Promise<void> {
        event.preventDefault();
        busy = true;
        error = '';
        try {
            await onsave({ baseUrl, bearerToken });
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            busy = false;
        }
    }

    async function switchToLocal(): Promise<void> {
        busy = true;
        error = '';
        try {
            await onuselocal();
        } catch (reason) {
            error = userFacingMessage(reason);
        } finally {
            busy = false;
        }
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell connection-settings"
        role="dialog"
        aria-modal="true"
        aria-labelledby="connection-settings-title"
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <div>
                <h2 id="connection-settings-title">Server connection</h2>
                <span class:remote={settings.mode === 'remote'} class="connection-mode">{settings.mode}</span>
            </div>
            <button class="icon-button" type="button" aria-label="Close server settings" onclick={oncancel}>
                <Icon name="close" size={16} />
            </button>
        </header>

        <form onsubmit={submit}>
            <label>
                <span>Server URL</span>
                <input
                    aria-label="Server URL"
                    type="url"
                    bind:value={baseUrl}
                    placeholder="https://sampler.example.net"
                    autocomplete="off"
                    required
                />
            </label>
            <label>
                <span>Access token</span>
                <input
                    aria-label="Access token"
                    type="password"
                    bind:value={bearerToken}
                    placeholder={settings.tokenConfigured ? 'Stored in system credential manager' : ''}
                    autocomplete="new-password"
                    minlength="32"
                    required
                />
            </label>

            <div class="connection-security">
                <Icon name="lock" size={15} />
                <span>System credential store</span>
            </div>

            {#if error}
                <p class="connection-settings-error" role="alert">{error}</p>
            {/if}

            <footer class="dialog-footer">
                <button class="secondary-button" type="button" disabled={busy} onclick={() => void switchToLocal()}>
                    {settings.mode === 'remote' ? 'Use local server' : 'Restart local server'}
                </button>
                <div class="connection-settings-dialog-actions">
                    <button class="secondary-button" type="button" disabled={busy} onclick={oncancel}>Cancel</button>
                    <button class="primary-button" type="submit" disabled={busy || !baseUrl || !bearerToken}>
                        {busy ? 'Connecting' : 'Connect'}
                    </button>
                </div>
            </footer>
        </form>
    </div>
</div>
