export interface AutocompleteOutsideDismissOptions {
    expanded: boolean;
    ondismiss: () => void;
}

export function dismissAutocompleteFromOutsidePointer(
    node: HTMLElement,
    initialOptions: AutocompleteOutsideDismissOptions,
) {
    let options = initialOptions;
    const dismiss = (event: PointerEvent): void => {
        if (!options.expanded || event.composedPath().includes(node)) return;
        options.ondismiss();
    };

    window.addEventListener('pointerdown', dismiss, true);
    return {
        update(nextOptions: AutocompleteOutsideDismissOptions): void {
            options = nextOptions;
        },
        destroy(): void {
            window.removeEventListener('pointerdown', dismiss, true);
        },
    };
}
