export function filesBackgroundSelection(node: HTMLElement, clear: () => void): { destroy(): void } {
    const click = (event: MouseEvent): void => {
        const target = event.target;
        const workspace = node.closest('[data-workspace-mode]') ?? node;
        if (
            event.button !== 0 ||
            event.defaultPrevented ||
            !(target instanceof HTMLElement) ||
            !workspace.contains(target) ||
            !target.hasAttribute('data-workspace-background') ||
            document.querySelector('[role="dialog"], [role="menu"]') ||
            window.getSelection()?.type === 'Range'
        )
            return;
        const rect = target.getBoundingClientRect();
        // A scrollbar click is not a click on the content background.
        if (
            target.clientWidth &&
            (event.clientX >= rect.left + target.clientLeft + target.clientWidth ||
                event.clientY >= rect.top + target.clientTop + target.clientHeight)
        )
            return;
        clear();
    };
    document.addEventListener('click', click);
    return { destroy: () => document.removeEventListener('click', click) };
}
