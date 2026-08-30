import { tick } from 'svelte';

import type { ObjectSelectionMode } from './objectSelection';

export type CollectionRevealAlignment = 'nearest' | 'center';

function collectionContainer(workspace: HTMLElement, collection: string): HTMLElement | undefined {
    return [...workspace.querySelectorAll<HTMLElement>('[data-collection-list]')].find(
        (candidate) => candidate.dataset.collectionList === collection,
    );
}

function collectionTarget(container: HTMLElement, objectId: string): HTMLElement | undefined {
    return [...container.querySelectorAll<HTMLElement>('[data-collection-object-id]')].find(
        (candidate) => candidate.dataset.collectionObjectId === objectId,
    );
}

export function linearNavigationIndex(
    key: string,
    currentIndex: number,
    itemCount: number,
    pageStep = 0,
): number | null {
    if (itemCount <= 0 || currentIndex < 0 || currentIndex >= itemCount) return null;
    let targetIndex: number | null = null;
    if (key === 'ArrowDown') targetIndex = Math.min(itemCount - 1, currentIndex + 1);
    if (key === 'ArrowUp') targetIndex = Math.max(0, currentIndex - 1);
    if (key === 'PageDown' && pageStep > 0) targetIndex = Math.min(itemCount - 1, currentIndex + pageStep);
    if (key === 'PageUp' && pageStep > 0) targetIndex = Math.max(0, currentIndex - pageStep);
    if (key === 'Home') targetIndex = 0;
    if (key === 'End') targetIndex = itemCount - 1;
    return targetIndex;
}

export function collectionPageStep(currentTarget: EventTarget | null): number {
    const current = currentTarget instanceof HTMLElement ? currentTarget : null;
    const container = current?.closest<HTMLElement>('[data-navigation-list]');
    const measuredExtent = current?.offsetHeight ?? 0;
    if (!container || container.clientHeight <= 0 || measuredExtent <= 0) return 1;
    return Math.max(1, Math.floor(container.clientHeight / measuredExtent) - 1);
}

export function keyboardSelectionMode(event: KeyboardEvent): ObjectSelectionMode {
    if (!event.shiftKey) return 'replace';
    return event.ctrlKey || event.metaKey ? 'add-range' : 'range';
}

export function hasDisallowedNavigationModifier(event: KeyboardEvent): boolean {
    return event.altKey || ((event.ctrlKey || event.metaKey) && !event.shiftKey);
}

export async function focusCollectionIndex(
    currentTarget: EventTarget | null,
    targetIndex: number,
    listOffset = 0,
): Promise<void> {
    const current = currentTarget instanceof HTMLElement ? currentTarget : null;
    const currentContainer = current?.closest<HTMLElement>('[data-navigation-list]');
    const workspace = currentContainer?.closest<HTMLElement>('[data-navigation-workspace]');
    const lists = workspace ? [...workspace.querySelectorAll<HTMLElement>('[data-navigation-list]')] : [];
    const currentListIndex = currentContainer ? lists.indexOf(currentContainer) : -1;
    const container = listOffset === 0 ? currentContainer : lists[currentListIndex + listOffset];
    if (!container) return;

    await tick();
    const target = [...container.querySelectorAll<HTMLElement>('[data-navigation-index]')].find(
        (candidate) => Number(candidate.dataset.navigationIndex) === targetIndex,
    );
    target?.focus({ preventScroll: true });
    target?.scrollIntoView?.({ block: 'nearest', inline: 'nearest', behavior: 'auto' });
}

export async function revealCollectionObject(
    workspace: HTMLElement,
    collection: string,
    objectId: string,
    alignment: CollectionRevealAlignment = 'nearest',
    focusTarget = false,
): Promise<boolean> {
    await tick();

    const container = collectionContainer(workspace, collection);
    if (!container) return false;
    const target = collectionTarget(container, objectId);
    if (!target) return false;
    if (focusTarget) target.focus({ preventScroll: true });
    target.scrollIntoView?.({ block: alignment, inline: 'nearest', behavior: 'auto' });
    return true;
}
