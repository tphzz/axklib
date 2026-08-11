import { describe, expect, it, vi } from 'vitest';
import type { ImageTransport } from '../../lib/transport';
import { ExportWorkflow } from './workflow.svelte';

describe('ExportWorkflow', () => {
    it('reports nonfatal package preservation warnings after export', async () => {
        const setStatus = vi.fn();
        const run = vi.fn().mockImplementation(async (begin: () => Promise<unknown>) => {
            await begin();
            return {
                status: 'completed',
                result: {
                    imageId: 'image-one',
                    revision: 4,
                    sourceMediaKind: 'A3K_ARCHIVE',
                    schemaVersion: '1.0',
                    packageId: 'package-one',
                    packageKind: 'VOLUME',
                    requiredExtension: '.axkvol',
                    destination: 'WORKSPACE',
                    output: { rootId: 'workspace', relativePath: 'A3000.axkvol' },
                    download: null,
                    roots: [],
                    objects: [],
                    relationships: [],
                    relationshipCount: 0,
                    totalPayloadBytes: 4096,
                    payloadsVerified: true,
                    valid: true,
                    sizeBytes: 8192,
                    issues: [
                        {
                            code: 'SEQUENCE_PAYLOAD_PRESERVED_OPAQUE',
                            message: "Sequence 'Demo' was preserved byte-for-byte; sampler playability is not verified",
                            fatal: false,
                        },
                    ],
                },
            };
        });
        const workflow = new ExportWorkflow({
            transport: {
                startImagePackageExport: vi.fn().mockResolvedValue({ status: 'queued' }),
                deleteRetainedPackage: vi.fn(),
            } as unknown as ImageTransport,
            jobs: { run } as never,
            picker: {} as never,
            isDesktop: false,
            sessionId: () => 12,
            imageLocation: () => null,
            setStatus,
            requestCompanionDisks: vi.fn(),
        });
        workflow.requestPackage([
            {
                kind: 'VOLUME',
                contentId: 'volume-a3000',
                name: 'A3000',
                partitionIndex: 0,
                volumeName: 'A3000',
                typeLabel: 'Volume',
            },
        ]);

        await workflow.runPackage({
            kind: 'WORKSPACE',
            output: { rootId: 'workspace', relativePath: 'A3000.axkvol' },
            overwrite: false,
        });

        expect(setStatus).toHaveBeenLastCalledWith(
            "Exported A3000; warning: Sequence 'Demo' was preserved byte-for-byte; sampler playability is not verified",
        );
        expect(workflow.packageRequest).toBeNull();
    });
});
