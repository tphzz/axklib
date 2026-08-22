import { invoke } from '@tauri-apps/api/core';

export interface AllocationInspectorRequest {
    imageId: string;
    revision: number;
    partitionIndex: number;
    partitionName: string;
}

export interface AllocationMapSaveRequest {
    suggestedName: string;
    document: unknown;
}

type ServerConnection = NonNullable<Window['__AXKLIB_SERVER__']>;

export async function resolveAllocationServerConnection(): Promise<ServerConnection | null> {
    if (window.__AXKLIB_SERVER__) return window.__AXKLIB_SERVER__;
    const connection = await invoke<ServerConnection | null>('server_connection');
    window.__AXKLIB_SERVER__ = connection ?? undefined;
    return connection;
}

export function formatAllocationBytes(bytes: number): string {
    if (bytes < 1024) return `${bytes.toLocaleString()} B`;
    const units = ['KiB', 'MiB', 'GiB'];
    let value = bytes / 1024;
    let unit = units[0];
    for (let index = 1; index < units.length && value >= 1024; index += 1) {
        value /= 1024;
        unit = units[index];
    }
    return `${value.toLocaleString(undefined, { maximumFractionDigits: 1 })} ${unit}`;
}

export function allocationSpaceStatistic(
    clusters: number,
    clusterSizeBytes: number,
): { primary: string; secondary: string } {
    return {
        primary: formatAllocationBytes(clusters * clusterSizeBytes),
        secondary: `${clusters.toLocaleString()} ${clusters === 1 ? 'cluster' : 'clusters'}`,
    };
}

export function allocationExportFilename(partitionName: string, partitionIndex: number): string {
    const partition = partitionName.replaceAll(/[^A-Za-z0-9._-]+/g, '_').replaceAll(/^_+|_+$/g, '');
    return `${partition || `partition-${partitionIndex + 1}`}-allocation-map.json`;
}

export async function openAllocationInspector(request: AllocationInspectorRequest): Promise<void> {
    await invoke('open_allocation_inspector', { request });
}

export async function saveAllocationMap(suggestedName: string, document: unknown): Promise<string | null> {
    return invoke<string | null>('save_allocation_map_json', {
        request: { suggestedName, document } satisfies AllocationMapSaveRequest,
    });
}
