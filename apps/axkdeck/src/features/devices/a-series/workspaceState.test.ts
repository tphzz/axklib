import { describe, expect, it } from 'vitest';
import type { CatalogWorkflow } from '../../catalog/workflow.svelte';
import type { AuditionWorkflow } from '../../audition/workflow.svelte';
import { captureASeriesWorkspace } from './workspaceState';
import { ViewMemory } from '../../workspace/viewMemory';

describe('document presentation state', () => {
    it('restores selections and searches without retaining stale native objects', () => {
        const catalog = {
            selectedProgramId: 'retained',
            selectedSampleId: 'removed',
            inspectorObjectId: 'retained',
            editorObjectIds: { programs: 'retained', samples: 'removed' },
            objectsById: new Map([['retained', {}]]),
        } as unknown as CatalogWorkflow;
        const audition = {
            laneQueries: { programs: { primary: 'brass', secondary: '', tertiary: '' } },
            showOnlyStandaloneSamples: false,
        } as unknown as AuditionWorkflow;
        let view = '';
        const restore = captureASeriesWorkspace(catalog, audition, 'programs', (next) => (view = next));
        catalog.selectedProgramId = '';
        audition.laneQueries.programs.primary = 'other';
        restore();
        expect(catalog.selectedProgramId).toBe('retained');
        expect(catalog.selectedSampleId).toBe('');
        expect(catalog.editorObjectIds.samples).toBe('');
        expect(audition.laneQueries.programs.primary).toBe('brass');
        expect(view).toBe('programs');
    });

    it('isolates document view slots and forgets them when the source set closes', () => {
        const memory = new ViewMemory();
        const first = memory.forDocument(1, 'first')('view', () => ({ query: '' }));
        first.query = 'piano';
        expect(memory.forDocument(1, 'second')('view', () => ({ query: '' })).query).toBe('');
        expect(memory.forDocument(1, 'first')('view', () => ({ query: '' })).query).toBe('piano');
        memory.forDocument(null, null);
        expect(memory.forDocument(1, 'first')('view', () => ({ query: '' })).query).toBe('');
    });
});
