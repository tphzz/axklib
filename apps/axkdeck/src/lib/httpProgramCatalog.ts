import type { AxklibHttpApiClient } from './httpApiClient';
import type { ProgramEditorCatalog } from './objectEditing';

const catalogs = new WeakMap<AxklibHttpApiClient, Promise<ProgramEditorCatalog>>();
export function loadProgramEditorCatalog(client: AxklibHttpApiClient): Promise<ProgramEditorCatalog> {
    let pending = catalogs.get(client);
    if (!pending) {
        pending = client.request<ProgramEditorCatalog>('GET', '/program-editor-catalog').catch((error) => {
            catalogs.delete(client);
            throw error;
        });
        catalogs.set(client, pending);
    }
    return pending;
}
