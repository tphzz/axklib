<script lang="ts">
    import type { FilesystemRootCapabilities } from '../../lib/filesystem';
    import AttributeHelp from '../../lib/components/AttributeHelp.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import { filesystemNameError, normalizeFilesystemName } from './nameValidation';

    let {
        value,
        label,
        capabilities,
        onchange,
        disabled = false,
        immediate = false,
        error,
        title = '',
        initialFocus,
        contextKey = '',
    }: {
        value: string;
        label: string;
        capabilities: FilesystemRootCapabilities;
        onchange: (name: string) => void;
        disabled?: boolean;
        immediate?: boolean;
        error?: string | null;
        title?: string;
        initialFocus?: 'select' | 'caret';
        contextKey?: string;
    } = $props();
    const id = $props.id();
    let touched = $state(false);
    const visible = $derived(!disabled && (immediate || touched || !!value));
    const rejection = $derived(error ?? filesystemNameError(value, capabilities));
    const explanation = $derived(
        rejection ??
            (capabilities.namePolicy === 'FAT_8_3_UPPERCASE'
                ? 'Valid FAT 8.3 name. ASCII letters are stored in uppercase.'
                : 'Valid filesystem name.'),
    );
    $effect(() => {
        contextKey;
        touched = false;
    });

    function input(event: Event & { currentTarget: HTMLInputElement }) {
        const control = event.currentTarget;
        const start = control.selectionStart;
        const end = control.selectionEnd;
        const direction = control.selectionDirection;
        const name = normalizeFilesystemName(control.value, capabilities);
        control.value = name;
        control.setSelectionRange(start, end, direction ?? undefined);
        touched = true;
        onchange(name);
    }
</script>

<span class="filesystem-name-field">
    <input
        class="dialog-field-control"
        aria-label={label}
        aria-invalid={visible && !!rejection}
        aria-describedby={visible ? `${id}-validation` : undefined}
        {value}
        {title}
        {disabled}
        autocomplete="off"
        data-dialog-initial-focus={initialFocus}
        oninput={input}
        onblur={() => (touched = true)}
    />
    <span class="name-validation" class:rejected={!!rejection}>
        {#if visible}
            <AttributeHelp
                label={`${label}: ${rejection ? 'Rejected' : 'Valid'}`}
                description={explanation}
                contextKey={`${contextKey}:${value}`}
            >
                <Icon name={rejection ? 'triangle-alert' : 'check'} size={14} />
            </AttributeHelp>
            <span id={`${id}-validation`} class="sr-only">{explanation}</span>
        {/if}
    </span>
</span>

<style>
    .filesystem-name-field {
        position: relative;
        display: block;
        min-width: 0;
        width: 100%;
    }
    .filesystem-name-field input {
        width: 100%;
        padding-right: 30px;
    }
    .name-validation {
        position: absolute;
        right: 6px;
        top: 0;
        bottom: 0;
        width: 18px;
        display: flex;
        align-items: center;
        justify-content: center;
        color: var(--color-success);
    }
    .name-validation.rejected {
        color: var(--color-danger);
    }
</style>
