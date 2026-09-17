import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpJobController } from './httpJobController';
import type { InputFileLocation } from './storageLocations';
import { serverInput } from './httpTransportWire';
import type { JobState } from './transport';
import type { FilesystemEdit } from './filesystem';

export function filesystemEditWire(edit: FilesystemEdit) {
    return edit.kind === 'PUT_FILE'
        ? {
              ...edit,
              source:
                  edit.source.kind === 'image-entry'
                      ? { imageEntryRef: edit.source.reference }
                      : serverInput(edit.source),
          }
        : edit;
}

export async function inspectFilesystemInputs(
    client: AxklibHttpApiClient,
    jobs: HttpJobController,
    inputs: InputFileLocation[],
): Promise<JobState> {
    if (!inputs.length || inputs.length > 10000) throw new Error('Choose between 1 and 10000 import inputs');
    const job = await client.invoke<never>('filesystem.inputs.inspect', { inputs: inputs.map(serverInput) });
    if (!jobs.isJob(job)) throw new Error('filesystem.inputs.inspect did not return a job');
    return jobs.map(job);
}

export async function inspectFilesystemImage(
    client: AxklibHttpApiClient,
    jobs: HttpJobController,
    source: InputFileLocation,
): Promise<JobState> {
    const job = await client.invoke<never>('filesystem.images.inspect', { source: serverInput(source) });
    if (!jobs.isJob(job)) throw new Error('filesystem.images.inspect did not return a job');
    return jobs.map(job);
}
