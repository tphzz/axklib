import type { CatalogWorkflow } from '../../catalog/workflow.svelte';
import type { AuditionWorkflow } from '../../audition/workflow.svelte';
import type { WorkspaceView } from '../../../lib/types';

const selections = [
    'selectedProgramId',
    'selectedSequenceId',
    'selectedBankId',
    'selectedBankMemberId',
    'selectedSampleId',
    'selectedBankWaveDataId',
    'selectedSampleWaveDataId',
    'selectedWaveDataId',
    'inspectorObjectId',
] as const;

export function resetASeriesWorkspace(audition: AuditionWorkflow, selectView: (view: WorkspaceView) => void): void {
    for (const key of Object.keys(audition.laneQueries) as WorkspaceView[])
        audition.laneQueries[key] = { primary: '', secondary: '', tertiary: '' };
    audition.showOnlyStandaloneSamples = true;
    selectView('programs');
}

export function captureASeriesWorkspace(
    catalog: CatalogWorkflow,
    audition: AuditionWorkflow,
    view: WorkspaceView,
    selectView: (view: WorkspaceView) => void,
): () => void {
    const ids = selections.map((key) => [key, catalog[key]] as const);
    const editors = { ...catalog.editorObjectIds };
    const queries = Object.entries(audition.laneQueries).map(([key, value]) => [key, { ...value }] as const);
    const standalone = audition.showOnlyStandaloneSamples;
    return () => {
        for (const [key, id] of ids) catalog[key] = catalog.objectsById.has(id) ? id : '';
        for (const view of Object.keys(editors) as WorkspaceView[])
            catalog.editorObjectIds[view] = catalog.objectsById.has(editors[view]) ? editors[view] : '';
        for (const [key, value] of queries) audition.laneQueries[key as WorkspaceView] = { ...value };
        audition.showOnlyStandaloneSamples = standalone;
        selectView(view);
    };
}
