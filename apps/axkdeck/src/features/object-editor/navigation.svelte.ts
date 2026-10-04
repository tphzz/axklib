export class EditorNavigation {
    tab = $state('trim-loop');
    page = $state('');
    effectSlot = $state(1);
    readonly scrollPositions = new Map<string, { top: number; left: number }>();

    selectTab(id: string): void {
        this.tab = id;
        this.page = '';
    }
}
