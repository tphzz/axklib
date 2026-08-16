import { invoke } from '@tauri-apps/api/core';

export interface AllocationInspectorRequest {
    imageId: string;
    revision: number;
    partitionIndex: number;
    partitionName: string;
}

export async function openAllocationInspector(request: AllocationInspectorRequest): Promise<void> {
    await invoke('open_allocation_inspector', { request });
}
