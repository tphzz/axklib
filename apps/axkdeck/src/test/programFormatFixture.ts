import type { components } from '../lib/generated/axklibApiV1';

export function programFormatFixture(current = false): components['schemas']['ProgramFormatMetadata'] {
    return {
        format: current ? 'A4000_A5000' : 'A3000',
        structurallyValid: true,
        headerRevision: current ? 4 : 2,
        logicalSize: current ? 912 : 736,
        storedAssignmentCount: 3,
        assignmentCapacity: 8,
        parameterTailBytes: current ? 176 : 0,
    };
}

export function programConversionFixture(current = false): components['schemas']['ProgramFormatConversion'] {
    return {
        kind: 'PROGRAM',
        payloadSha256: (current ? 'b' : 'a').repeat(64),
        partitionIndex: 0,
        volumeName: 'Volume',
        programNumber: 33,
        programName: 'Test',
        programFormat: programFormatFixture(current),
        canConvertFormat: true,
        reason: '',
        formatConversions: [
            { targetFormat: current ? 'A3000' : 'A4000_A5000', allowed: true, changes: [], blockers: [] },
        ],
    };
}
