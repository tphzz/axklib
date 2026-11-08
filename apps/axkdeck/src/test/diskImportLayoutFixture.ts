import type { FloppyImportWorkflow } from '../features/import/floppyWorkflow.svelte';
import type { ImportCompletion } from '../features/import/importCompletion.svelte';
import { volumeCapacityFixture } from './volumeCapacityFixture';
import { ASeriesPreferences, provideASeriesPreferences } from '../lib/aSeriesPreferences.svelte';

export function diskLayoutCapacity(blocked = false) {
    const report = volumeCapacityFixture(blocked ? 'DOES_NOT_FIT' : 'FITS');
    report.volumeName = 'Existing';
    for (const profile of report.profiles) {
        if (!blocked) {
            profile.peakBytes = 368 * 1024;
            profile.peakSlots = 489;
        }
    }
    return { target: 'A4000_A5000' as const, allowed: !blocked, reports: [report] };
}

export function diskLayoutCompletion(completion: ImportCompletion, scenario: string) {
    const target = scenario === 'a3k' ? 'A3000' : 'A4000_A5000';
    const preferences = new ASeriesPreferences({ load: async () => target, save: async () => {} });
    preferences.generation = target;
    provideASeriesPreferences(preferences);
    completion.capacity.target = target;
    Object.assign(window, {
        diskImportLayout: {
            showWarnings: () => {
                completion.phase = 'warnings';
                completion.message = 'Imported with 1 warning';
                completion.warnings = ['Import saved; journal cleanup is pending. Retain recovery data.'];
            },
        },
    });
    if (['importing', 'unconfirmed', 'refresh-failed', 'warnings'].includes(scenario)) {
        completion.phase = scenario as typeof completion.phase;
        completion.message =
            scenario === 'unconfirmed'
                ? 'Import outcome unconfirmed; do not repeat the import'
                : scenario === 'refresh-failed'
                  ? 'Import saved; refresh failed'
                  : scenario === 'warnings'
                    ? 'Imported with 1 warning'
                    : 'Importing';
        if (scenario === 'warnings')
            completion.warnings = ['Import saved; journal cleanup is pending. Retain recovery data.'];
    }
}

export function configureFloppyLayout(workflow: FloppyImportWorkflow, scenario: string) {
    const request = workflow.request!;
    diskLayoutCompletion(workflow.completion, scenario);
    request.mode = 'existing';
    request.partitionIndex = 0;
    request.volumeName = 'Existing';
    request.inspection!.label = scenario === 'a3k' ? 'A3000 Demo' : 'A4000/A5000 Demo';
    request.members[0].name = 'a4k-demo-disk01.ima';
    if (request.plan) {
        request.plan.capacity = diskLayoutCapacity(scenario === 'capacity');
        request.plan.programSlotPlacements = [
            {
                placementId: 'placement-1',
                partitionIndex: 0,
                volumeName: 'Existing',
                mode: 'CONTIGUOUS',
                applied: true,
                suggestedStartSlot: 10,
                requiredSlotCount: 4,
                availableSlotCount: 119,
                occupiedRanges: [{ first: 1, last: 9 }],
                sourceRanges: [{ first: 1, last: 4 }],
                destinationRanges: [{ first: 10, last: 13 }],
                mappings: [1, 2, 3, 4].map((slot) => ({
                    packageIndex: 0,
                    nodeId: `program-${slot}`,
                    sourceSlot: slot,
                    destinationSlot: slot + 9,
                    requiresUserAction: false,
                })),
            },
        ];
        if (scenario === 'a3k') {
            workflow.completion.capacity.target = 'A3000';
            request.plan.capacity.target = 'A3000';
        }
        if (scenario === 'conflicts') {
            request.plan.valid = false;
            request.plan.opaqueSequences = [
                { packageIndex: 0, nodeId: 'sequence-1', name: 'DEMO SEQUENCE', action: null },
            ];
        }
    }
    if (scenario === 'empty') {
        request.members = [];
        request.inspection = null;
        request.plan = null;
        request.selected = [];
        request.dirty = true;
    }
    if (scenario === 'planning') {
        request.status = 'planning';
        request.dirty = true;
    }
}
