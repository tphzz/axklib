<script lang="ts">
    import {
        collectionPageStep,
        focusCollectionIndex,
        hasDisallowedNavigationModifier,
        keyboardSelectionMode,
        linearNavigationIndex,
    } from '../collectionNavigation';
    import {
        emptyPackageExportSelection,
        selectionMode,
        updatePackageExportSelection,
        type PackageExportSelectionState,
    } from '../objectSelection';
    import { objectSizeSummary, objectSizeTooltip } from '../objectSizePresentation';
    import type { SystemProgramContexts, SystemProgramPart } from '../transport';
    import type { ObjectRenameTarget, PackageExportObject, Program } from '../types';
    import CollectionToolbar from './CollectionToolbar.svelte';
    import Icon from './Icon.svelte';
    import ObjectContextMenu from './ObjectContextMenu.svelte';

    export type ProgramPresentation = 'single' | 'multi';

    interface Props {
        programs: Program[];
        contexts: SystemProgramContexts | null;
        contextsLoading: boolean;
        contextsError: string;
        presentation: ProgramPresentation;
        selectedPartNumber: number | null;
        activeObjectId: string;
        query: string;
        onquerychange: (value: string) => void;
        onpresentationchange: (value: ProgramPresentation) => void;
        onprogramselect: (program: Program) => void;
        onpartselect: (part: SystemProgramPart, program: Program | null) => void;
        objectRenameAvailable?: boolean;
        onrenameobject?: (target: ObjectRenameTarget) => void;
        objectDeletionAvailable?: boolean;
        ondeleteobjects?: (objects: PackageExportObject[]) => void;
        programGenerationAvailable?: boolean;
        onprogramgeneration?: () => void;
        packageExportAvailable?: boolean;
        onexportobjects?: (objects: PackageExportObject[]) => void;
        selection?: PackageExportSelectionState;
        onselectionchange?: (selection: PackageExportSelectionState) => void;
        onselectionlimit?: () => void;
    }

    let {
        programs,
        contexts,
        contextsLoading,
        contextsError,
        presentation,
        selectedPartNumber,
        activeObjectId,
        query,
        onquerychange,
        onpresentationchange,
        onprogramselect,
        onpartselect,
        objectRenameAvailable = false,
        onrenameobject = () => undefined,
        objectDeletionAvailable = false,
        ondeleteobjects = () => undefined,
        programGenerationAvailable = false,
        onprogramgeneration = () => undefined,
        packageExportAvailable = false,
        onexportobjects = () => undefined,
        selection = emptyPackageExportSelection(),
        onselectionchange = () => undefined,
        onselectionlimit = () => undefined,
    }: Props = $props();

    let objectMenu = $state<{ program: Program; objects: PackageExportObject[]; left: number; top: number } | null>(
        null,
    );
    let systemInfoOpen = $state(false);
    let systemInfoPinned = $state(false);
    let systemInfoTrigger: HTMLButtonElement | undefined = $state();

    const normalizedQuery = $derived(query.trim().toLocaleLowerCase());
    const filteredPrograms = $derived(
        normalizedQuery
            ? programs.filter((item) => `${item.slot} ${item.name}`.toLocaleLowerCase().includes(normalizedQuery))
            : programs,
    );
    const system2Context = $derived(contexts?.files.find((context) => context.fileKind === 'SYSTEM2') ?? null);
    const availableSystem2 = $derived(
        system2Context?.fileKind === 'SYSTEM2' && system2Context.availability === 'AVAILABLE' ? system2Context : null,
    );
    const a3000Context = $derived(contexts?.files.find((context) => context.fileKind === 'SYSTEM') ?? null);
    const availableA3000 = $derived(
        a3000Context?.fileKind === 'SYSTEM' && a3000Context.availability === 'AVAILABLE' ? a3000Context : null,
    );
    const visibleSystemContexts = $derived(
        contexts?.files.filter((context) => context.availability !== 'NOT_PRESENT') ?? [],
    );
    const filteredParts = $derived(
        availableSystem2
            ? availableSystem2.parts.filter((part) => {
                  const assigned = programFor(part.programNumber);
                  const role = part.master ? 'Master' : '';
                  return `${part.partLabel} ${programLabel(part.programNumber, assigned)} ${role}`
                      .toLocaleLowerCase()
                      .includes(normalizedQuery);
              })
            : [],
    );
    const count = $derived(presentation === 'single' ? programs.length : (availableSystem2?.parts.length ?? 0));
    const countText = $derived(presentation === 'multi' ? `${count} ${count === 1 ? 'part' : 'parts'}` : undefined);
    const multiUnavailableMessage = $derived.by(() => {
        if (availableA3000 && (!system2Context || system2Context.availability === 'NOT_PRESENT')) {
            return 'A3000 SYSTEM stores receive settings, but it does not contain Program Mode or Multi Part assignments.';
        }
        if (contexts?.message) return contexts.message;
        if (system2Context && system2Context.availability !== 'AVAILABLE') return system2Context.message;
        return 'Multi assignments are unavailable.';
    });

    $effect(() => {
        if (visibleSystemContexts.length !== 0) return;
        systemInfoOpen = false;
        systemInfoPinned = false;
    });

    function enabledLabel(enabled: boolean): string {
        return enabled ? 'On' : 'Off';
    }

    function showSystemInfo(): void {
        systemInfoOpen = true;
    }

    function hideSystemInfoPreview(): void {
        if (!systemInfoPinned) systemInfoOpen = false;
    }

    function toggleSystemInfo(event: MouseEvent): void {
        event.stopPropagation();
        systemInfoPinned = !systemInfoPinned;
        systemInfoOpen = systemInfoPinned;
    }

    function closeSystemInfo(restoreFocus = false): void {
        if (!systemInfoOpen && !systemInfoPinned) return;
        systemInfoPinned = false;
        if (restoreFocus && document.activeElement !== systemInfoTrigger) systemInfoTrigger?.focus();
        systemInfoOpen = false;
    }

    function handleWindowKeydown(event: KeyboardEvent): void {
        if (event.key === 'Escape' && systemInfoOpen) closeSystemInfo(true);
    }

    function programFor(programNumber: number): Program | null {
        return programs.find((program) => program.programNumber === programNumber) ?? null;
    }

    function programSlot(programNumber: number): string {
        return String(programNumber).padStart(3, '0');
    }

    function programName(programNumber: number, program: Program | null): string {
        const slot = programSlot(programNumber);
        return program?.name ?? `Pgm ${slot}`;
    }

    function programLabel(programNumber: number, program: Program | null): string {
        return `${programSlot(programNumber)}: ${programName(programNumber, program)}`;
    }

    function exportProgram(program: Program): PackageExportObject {
        return {
            kind: 'PROGRAM',
            objectId: program.objectId,
            name: program.name,
            typeLabel: 'Program',
            partitionIndex: program.object.partitionIndex,
            partitionName: program.object.partitionName,
            volumeName: program.object.volumeName,
        };
    }

    function updateSelection(program: Program, mode: Parameters<typeof updatePackageExportSelection>[5]): void {
        const domain = programs.map(exportProgram);
        const visible = filteredPrograms.map(exportProgram);
        const first = domain[0];
        const key = `programs\u0000${first?.partitionIndex ?? ''}\u0000${first?.volumeName ?? ''}`;
        const result = updatePackageExportSelection(selection, key, domain, visible, program.objectId, mode);
        if (result.limitExceeded) onselectionlimit();
        else onselectionchange(result.selection);
    }

    function selectProgram(event: MouseEvent, program: Program): void {
        const mode = selectionMode(event);
        updateSelection(program, mode);
        if (mode === 'replace') onprogramselect(program);
    }

    function navigatePrograms(event: KeyboardEvent, currentIndex: number): void {
        if (hasDisallowedNavigationModifier(event)) return;
        const targetIndex = linearNavigationIndex(
            event.key,
            currentIndex,
            filteredPrograms.length,
            collectionPageStep(event.currentTarget),
        );
        if (targetIndex === null) return;
        event.preventDefault();
        const target = filteredPrograms[targetIndex];
        if (!target) return;
        const mode = keyboardSelectionMode(event);
        updateSelection(target, mode);
        if (mode === 'replace') onprogramselect(target);
        void focusCollectionIndex(event.currentTarget, targetIndex);
    }

    function navigateParts(event: KeyboardEvent, currentIndex: number): void {
        if (hasDisallowedNavigationModifier(event)) return;
        const targetIndex = linearNavigationIndex(
            event.key,
            currentIndex,
            filteredParts.length,
            collectionPageStep(event.currentTarget),
        );
        if (targetIndex === null) return;
        event.preventDefault();
        if (targetIndex === currentIndex) return;
        const target = filteredParts[targetIndex];
        if (!target) return;
        onpartselect(target, programFor(target.programNumber));
        void focusCollectionIndex(event.currentTarget, targetIndex);
    }

    function renameTarget(program: Program): ObjectRenameTarget {
        return { kind: 'program', object: program.object, name: program.name, programNumber: program.programNumber };
    }

    function openMenu(event: MouseEvent, program: Program): void {
        if (!objectRenameAvailable && !objectDeletionAvailable && !packageExportAvailable) return;
        event.preventDefault();
        const exported = exportProgram(program);
        objectMenu = {
            program,
            objects: selection.items.some((item) => item.objectId === program.objectId) ? selection.items : [exported],
            left: Math.max(8, Math.min(event.clientX, window.innerWidth - 180)),
            top: Math.max(8, Math.min(event.clientY, window.innerHeight - 56)),
        };
    }
</script>

<svelte:window onclick={() => closeSystemInfo()} onkeydown={handleWindowKeydown} />

<section class="collection-panel" aria-label="Programs">
    {#snippet systemInfoControls()}
        {#if visibleSystemContexts.length > 0}
            <div
                class="program-system-info"
                role="group"
                aria-label="Saved System File information"
                onmouseenter={showSystemInfo}
                onmouseleave={hideSystemInfoPreview}
            >
                <button
                    bind:this={systemInfoTrigger}
                    class:active={systemInfoOpen}
                    class="program-system-info-trigger"
                    type="button"
                    aria-label="Saved System File details"
                    aria-haspopup="dialog"
                    aria-expanded={systemInfoOpen}
                    aria-controls="program-system-info-popover"
                    title="Saved System File details"
                    onclick={toggleSystemInfo}
                    onfocus={showSystemInfo}
                    onblur={hideSystemInfoPreview}
                >
                    <Icon name="info" size={12} />
                </button>
                {#if systemInfoOpen}
                    <div
                        id="program-system-info-popover"
                        class="program-system-info-popover"
                        role="dialog"
                        aria-label="Saved System File details"
                        tabindex="-1"
                        onclick={(event) => event.stopPropagation()}
                        onkeydown={handleWindowKeydown}
                    >
                        {#each visibleSystemContexts as context (context.fileKind)}
                            <section class="program-system-info-file">
                                <header>
                                    <strong>{context.fileKind}</strong>
                                    <span>{context.availability === 'AVAILABLE' ? context.model : 'Invalid'}</span>
                                </header>
                                {#if context.availability === 'AVAILABLE'}
                                    <dl>
                                        <div>
                                            <dt>Basic receive</dt>
                                            <dd>{context.basicReceive.display}</dd>
                                        </div>
                                        <div>
                                            <dt>Omni</dt>
                                            <dd>{enabledLabel(context.omni)}</dd>
                                        </div>
                                        <div>
                                            <dt>Program Change</dt>
                                            <dd>{enabledLabel(context.programChangeEnabled)}</dd>
                                        </div>
                                        {#if context.fileKind === 'SYSTEM2'}
                                            <div>
                                                <dt>Saved mode</dt>
                                                <dd>{context.savedProgramMode === 'SINGLE' ? 'Single' : 'Multi'}</dd>
                                            </div>
                                        {/if}
                                    </dl>
                                {:else}
                                    <p>{context.message}</p>
                                {/if}
                            </section>
                        {/each}
                    </div>
                {/if}
            </div>
        {/if}
    {/snippet}
    {#snippet presentationControls()}
        <span
            class="inline-flex overflow-hidden rounded-md border border-[var(--color-border)]"
            role="group"
            aria-label="Program presentation"
        >
            <button
                class="grid size-6 place-items-center bg-transparent text-[var(--color-text-muted)] hover:bg-[var(--color-panel-raised)] aria-pressed:bg-[var(--color-accent-strong)] aria-pressed:text-white"
                type="button"
                aria-label="Single Program view"
                aria-pressed={presentation === 'single'}
                title="Single Program view"
                onclick={() => onpresentationchange('single')}><Icon name="program-single" size={14} /></button
            >
            <button
                class="grid size-6 place-items-center border-l border-[var(--color-border)] bg-transparent text-[var(--color-text-muted)] hover:bg-[var(--color-panel-raised)] aria-pressed:bg-[var(--color-accent-strong)] aria-pressed:text-white"
                type="button"
                aria-label="Multi Part view"
                aria-pressed={presentation === 'multi'}
                title="Multi Part view"
                onclick={() => onpresentationchange('multi')}><Icon name="program-multi" size={14} /></button
            >
        </span>
    {/snippet}
    <CollectionToolbar
        title="Programs"
        {count}
        {countText}
        {query}
        {onquerychange}
        actionLabel={presentation === 'single' && programGenerationAvailable ? 'Generate Programs' : undefined}
        actionIcon="sparkles"
        onaction={onprogramgeneration}
        titleControls={systemInfoControls}
        trailingControls={presentationControls}
    />

    {#if presentation === 'single'}
        <div
            class:empty-collection={filteredPrograms.length === 0}
            class="collection-body program-list"
            data-navigation-list
        >
            {#each filteredPrograms as program, index (program.id)}
                <button
                    type="button"
                    class:active={activeObjectId === program.objectId}
                    class:selected={selection.items.some((item) => item.objectId === program.objectId)}
                    class="program-row"
                    data-navigation-index={index}
                    title={objectSizeTooltip(program.object)}
                    aria-pressed={selection.items.some((item) => item.objectId === program.objectId)}
                    onclick={(event) => selectProgram(event, program)}
                    oncontextmenu={(event) => openMenu(event, program)}
                    onkeydown={(event) => navigatePrograms(event, index)}
                >
                    <span class="object-slot">{program.slot}</span><strong>{program.name}</strong>
                    <small class="object-size-summary">{objectSizeSummary(program.object)}</small>
                </button>
            {:else}
                <p class="empty-copy">No matching Programs</p>
            {/each}
        </div>
    {:else}
        <div class="collection-body program-multi-list" data-navigation-list>
            {#if contextsLoading}
                <p class="empty-copy">Reading the partition's saved System Files…</p>
            {:else if contextsError}
                <p class="empty-copy">{contextsError}</p>
            {:else if !availableSystem2}
                <p class="empty-copy">{multiUnavailableMessage}</p>
            {:else}
                <div class="program-multi-table" role="region" aria-label="Multi Part assignments">
                    <div class="program-multi-header" aria-hidden="true">
                        <span>Part</span><span>Program</span><span>Role</span>
                    </div>
                    {#each filteredParts as part, index (part.partNumber)}
                        {@const assigned = programFor(part.programNumber)}
                        <button
                            type="button"
                            class:active={selectedPartNumber === part.partNumber}
                            data-navigation-index={index}
                            aria-label={`Part ${part.partLabel}, ${programLabel(part.programNumber, assigned)}${part.master ? ', Master' : ''}`}
                            aria-pressed={selectedPartNumber === part.partNumber}
                            onclick={() => onpartselect(part, assigned)}
                            onkeydown={(event) => navigateParts(event, index)}
                        >
                            <span class="program-multi-part">{part.partLabel}</span>
                            <span class="program-multi-program">
                                <span class="program-multi-program-slot">{programSlot(part.programNumber)}: </span>
                                <strong>{programName(part.programNumber, assigned)}</strong>
                            </span>
                            <span class="program-multi-role">{part.master ? 'Master' : '—'}</span>
                        </button>
                    {:else}
                        <p class="empty-copy">No matching Multi Parts</p>
                    {/each}
                </div>
            {/if}
        </div>
    {/if}
</section>

{#if objectMenu}
    <ObjectContextMenu
        objectName={objectMenu.objects.length === 1
            ? objectMenu.objects[0]!.name
            : `${objectMenu.objects.length} objects`}
        selectionCount={objectMenu.objects.length}
        left={objectMenu.left}
        top={objectMenu.top}
        onclose={() => (objectMenu = null)}
        onrename={objectRenameAvailable && objectMenu.objects.length === 1
            ? () => onrenameobject(renameTarget(objectMenu!.program))
            : undefined}
        onexportpackage={packageExportAvailable ? () => onexportobjects(objectMenu!.objects) : undefined}
        ondelete={objectDeletionAvailable ? () => ondeleteobjects(objectMenu!.objects) : undefined}
    />
{/if}
