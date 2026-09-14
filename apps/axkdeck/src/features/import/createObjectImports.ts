import { PackageImportWorkflow } from './packageWorkflow.svelte';
import { PackageBatchImportWorkflow } from './packageBatchWorkflow.svelte';
import { FloppyImportWorkflow } from './floppyWorkflow.svelte';
import type { PackageBatchImportDependencies } from './packageBatchTypes';

export function createObjectImports(
    dependencies: ConstructorParameters<typeof FloppyImportWorkflow>[0] & PackageBatchImportDependencies,
) {
    return {
        packageImportWorkflow: new PackageImportWorkflow(dependencies),
        packageBatchImportWorkflow: new PackageBatchImportWorkflow(dependencies),
        floppyImportWorkflow: new FloppyImportWorkflow(dependencies),
    };
}
