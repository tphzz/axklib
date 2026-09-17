import type { AxklibHttpApiClient } from './httpApiClient';
import type { HttpJobController } from './httpJobController';
import { randomIdempotencyKey, serverInput } from './httpTransportWire';
import type { Su700Request } from './su700Import';

export async function startSu700Import(
    client: AxklibHttpApiClient,
    jobs: HttpJobController,
    remoteId: (id: number) => string,
    request: Su700Request,
) {
    const destination = request.destination
        ? {
              imageId: remoteId(request.destination.sessionId),
              expectedRevision: request.destination.expectedRevision,
              rootEntryId: request.destination.rootEntryId,
              volumeName: request.destination.volumeName,
          }
        : null;
    const operation = request.expectedSource ? 'images.su700.import' : 'images.su700.import.inspect';
    const job = await client.invoke<never>(
        operation,
        {
            source: serverInput(request.source),
            destination,
            includedExtras: request.includedExtras,
            ...(request.expectedSource ? { expectedSource: request.expectedSource } : {}),
        },
        request.expectedSource ? { idempotencyKey: request.idempotencyKey ?? randomIdempotencyKey() } : {},
    );
    if (!jobs.isJob(job)) throw new Error(`${operation} did not return a job`);
    return jobs.map(job);
}
