import type { AuditionWorkflow } from '../audition/workflow.svelte';
import type { CatalogWorkflow } from '../catalog/workflow.svelte';
import type { DeletionWorkflow } from '../deletion/workflow.svelte';
import type { ExportWorkflow } from '../export/workflow.svelte';
import type { MediaExportWorkflow } from '../export/mediaWorkflow.svelte';
import type { VolumePackageExportWorkflow } from '../export/volumePackageWorkflow.svelte';
import type { VolumeFloppyExportWorkflow } from '../export/volumeFloppyWorkflow.svelte';
import type { PackageImportWorkflow } from '../import/packageWorkflow.svelte';
import type { FloppyImportWorkflow } from '../import/floppyWorkflow.svelte';
import type { MutationWorkflow } from '../mutation/workflow.svelte';
import type { ProgramGenerationWorkflow } from '../program-generation/workflow.svelte';
import type { ExtentLayoutRepairWorkflow } from './extentLayoutRepairWorkflow.svelte';

export interface SessionCollaborators {
    catalog: CatalogWorkflow;
    audition: AuditionWorkflow;
    mutation: MutationWorkflow;
    exports: ExportWorkflow;
    volumePackages: VolumePackageExportWorkflow;
    volumeFloppies: VolumeFloppyExportWorkflow;
    mediaExports: MediaExportWorkflow;
    packageImport: PackageImportWorkflow;
    floppyImport?: FloppyImportWorkflow;
    deletion: DeletionWorkflow;
    programGeneration: ProgramGenerationWorkflow;
    extentRepairs: ExtentLayoutRepairWorkflow;
    clearExportSelection: () => void;
    captureDocumentState?: () => (() => void) | undefined;
    resetDocumentState?: () => void;
}
