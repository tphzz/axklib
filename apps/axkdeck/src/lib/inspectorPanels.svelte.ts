import { getContext, setContext } from 'svelte';

const inspectorPanelsContext = Symbol('inspector-panels');
export const inspectorSectionVisibility = Symbol('inspector-section-visibility');

export class InspectorPanels {
    private collapsed = $state<Record<string, boolean>>({});

    expanded(scope: string, sectionId: string): boolean {
        const defaultCollapsed =
            sectionId === 'relationships' ||
            ((scope === 'sample' || scope === 'sample-bank') && sectionId === 'stored-format');
        return !(this.collapsed[`${scope}:${sectionId}`] ?? defaultCollapsed);
    }

    setExpanded(scope: string, sectionId: string, expanded: boolean): void {
        this.collapsed[`${scope}:${sectionId}`] = !expanded;
    }
}

export function provideInspectorPanels(panels?: InspectorPanels): InspectorPanels {
    return setContext(
        inspectorPanelsContext,
        panels ?? getContext<InspectorPanels | undefined>(inspectorPanelsContext) ?? new InspectorPanels(),
    );
}
