import { tick } from 'svelte';

import type { ObjectSelectionMode } from './objectSelection';

export const denseCollectionRowExtent = 26;
export const waveDataCollectionRowExtent = 42;

export type CollectionRevealAlignment = 'nearest' | 'center';

function clampScrollTop(container: HTMLElement, scrollTop: number): number {
    const maximum = container.scrollHeight - container.clientHeight;
    return Math.max(0, maximum > 0 ? Math.min(maximum, scrollTop) : scrollTop);
}

function centerRenderedTarget(container: HTMLElement, target: HTMLElement): void {
    const containerBounds = container.getBoundingClientRect();
    const targetBounds = target.getBoundingClientRect();
    if (containerBounds.height <= 0 || targetBounds.height <= 0) return;
    const targetCenter = targetBounds.top - containerBounds.top + targetBounds.height / 2;
    container.scrollTop = clampScrollTop(container, container.scrollTop + targetCenter - container.clientHeight / 2);
}

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

async function nextRenderFrame(): Promise<void> {
    if (typeof window === 'undefined' || typeof window.requestAnimationFrame !== 'function') {
        await tick();
        return;
    }
    await new Promise<void>((resolve) => window.requestAnimationFrame(() => resolve()));
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

export function collectionPageStep(currentTarget: EventTarget | null, itemExtent?: number): number {
    const current = currentTarget instanceof HTMLElement ? currentTarget : null;
    const container = current?.closest<HTMLElement>('[data-navigation-list]');
    const measuredExtent = itemExtent ?? current?.offsetHeight ?? 0;
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
    itemExtent?: number,
    listOffset = 0,
): Promise<void> {
    const current = currentTarget instanceof HTMLElement ? currentTarget : null;
    const currentContainer = current?.closest<HTMLElement>('[data-navigation-list]');
    const workspace = currentContainer?.closest<HTMLElement>('[data-navigation-workspace]');
    const lists = workspace ? [...workspace.querySelectorAll<HTMLElement>('[data-navigation-list]')] : [];
    const currentListIndex = currentContainer ? lists.indexOf(currentContainer) : -1;
    const container = listOffset === 0 ? currentContainer : lists[currentListIndex + listOffset];
    if (!container) return;

    if (itemExtent !== undefined) {
        const itemTop = targetIndex * itemExtent;
        const itemBottom = itemTop + itemExtent;
        const viewportBottom = container.scrollTop + container.clientHeight;
        if (itemTop < container.scrollTop) container.scrollTop = itemTop;
        else if (itemBottom > viewportBottom) {
            container.scrollTop = Math.max(0, itemBottom - Math.max(container.clientHeight, itemExtent));
        }
        container.dispatchEvent(new Event('scroll'));
    }

    await tick();
    const target = [...container.querySelectorAll<HTMLElement>('[data-navigation-index]')].find(
        (candidate) => Number(candidate.dataset.navigationIndex) === targetIndex,
    );
    target?.focus({ preventScroll: true });
    if (itemExtent === undefined) target?.scrollIntoView?.({ block: 'nearest' });
}

export async function revealCollectionObject(
    workspace: HTMLElement,
    collection: string,
    objectId: string,
    targetIndex: number,
    itemExtent?: number,
    alignment: CollectionRevealAlignment = 'nearest',
): Promise<boolean> {
    if (targetIndex < 0) return false;
    await tick();

    for (let attempt = 0; attempt < 3; attempt += 1) {
        const container = collectionContainer(workspace, collection);
        if (container) {
            if (itemExtent !== undefined) {
                const itemTop = targetIndex * itemExtent;
                const itemBottom = itemTop + itemExtent;
                if (alignment === 'center') {
                    container.scrollTop = clampScrollTop(
                        container,
                        itemTop - (container.clientHeight - itemExtent) / 2,
                    );
                } else {
                    const viewportBottom = container.scrollTop + container.clientHeight;
                    if (itemTop < container.scrollTop) container.scrollTop = itemTop;
                    else if (itemBottom > viewportBottom) {
                        container.scrollTop = Math.max(0, itemBottom - Math.max(container.clientHeight, itemExtent));
                    }
                }
                container.dispatchEvent(new Event('scroll'));
            }

            await tick();
            const target = collectionTarget(container, objectId);
            if (target) {
                target.focus({ preventScroll: true });
                if (alignment === 'center' && itemExtent === undefined) {
                    centerRenderedTarget(container, target);
                } else if (itemExtent === undefined) {
                    target.scrollIntoView?.({ block: 'nearest' });
                }
                return true;
            }
        }

        if (attempt === 2) break;
        await nextRenderFrame();
        await tick();
    }
    return false;
}
