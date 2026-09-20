export class EditorNavigation {
    tab = $state('trim-loop');
    page = $state('');

    selectTab(id: string): void {
        this.tab = id;
        this.page = '';
    }
}
