import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpJobController } from './httpJobController';
import type { SessionState } from './httpTransportModels';
import type { components } from './generated/axklibApiV1';
import type { VolumeCapacityInspection, VolumeDeletionInspection } from './transport';

async function inspect<T>(
    client: AxklibHttpApiClient,
    jobs: HttpJobController,
    session: SessionState,
    operation: string,
    input: Record<string, unknown>,
): Promise<T> {
    const result = await client.invoke<T>(operation, {
        imageId: session.remoteId,
        expectedRevision: session.revision,
        ...input,
    });
    if (jobs.isJob(result)) throw new Error(`${operation} unexpectedly returned a job`);
    return result;
}

export function inspectVolumeDeletion(
    client: AxklibHttpApiClient,
    jobs: HttpJobController,
    session: SessionState,
    targets: components['schemas']['ImageVolumeDeletionTarget'][],
): Promise<VolumeDeletionInspection> {
    return inspect(client, jobs, session, 'images.volume_deletion.inspect', { targets });
}

export function inspectVolumeCapacity(
    client: AxklibHttpApiClient,
    jobs: HttpJobController,
    session: SessionState,
    contentScopeId: string,
): Promise<VolumeCapacityInspection> {
    return inspect(client, jobs, session, 'images.volume_capacity.inspect', { contentScopeId });
}
