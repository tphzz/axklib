import { mount, unmount } from 'svelte';
import AttributeHelp from './AttributeHelp.svelte';

/** Reuses the shared tooltip on an existing control without nesting interactive elements. */
export function hoverHelp(anchor: HTMLElement, description: string) {
    const target = document.createElement('div');
    document.body.appendChild(target);
    const props = $state({ anchor, label: '', description });
    const component = mount(AttributeHelp, { target, props });
    return {
        update(value: string) {
            props.description = value;
        },
        destroy() {
            void unmount(component);
            target.remove();
        },
    };
}
