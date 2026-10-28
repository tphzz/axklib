import type { DeviceInspection } from '../lib/httpDeviceFormatting';
import type { DeviceSession } from '../lib/httpDevices';
import { deviceIdentity } from './deviceFixture';

export function deviceFormatSession(): DeviceSession {
    return {
        sessionId: 'format-session',
        candidate: {
            id: 'card',
            model: 'Synthetic removable disk',
            serial: 'CARD-TEST',
            sizeBytes: 32_000_000_000,
            sectorSize: 512,
            identity: deviceIdentity,
        },
        targets: [
            {
                id: 'disk',
                label: 'Whole disk',
                offsetBytes: 0,
                sizeBytes: 32_000_000_000,
                sectorSize: 512,
                readOnly: false,
                notices: [],
                scsi: null,
                recoveryConfirmationText: 'RESTORE /dev/synthetic',
            },
        ],
        writesSupported: true,
        writesAuthorized: false,
        invalidated: false,
    };
}

export function deviceFormatInspection(session = deviceFormatSession(), targetId = 'disk'): DeviceInspection {
    const target = session.targets.find((target) => target.id === targetId)!;
    const size = Math.min(target.sizeBytes, 8_589_934_592);
    return {
        inspectionToken: 'inspection-token',
        sessionId: session.sessionId,
        candidate: session.candidate,
        target,
        complete: true,
        blockedReasons: [],
        warnings: ['USB reader descriptors do not prove the identity of the inserted card.'],
        contents: [
            {
                kind: 'partition',
                type: 'MBR type 7',
                label: 'Old data',
                offsetBytes: target.offsetBytes + 1048576,
                sizeBytes: 1_048_576,
                validation: 'STRUCTURE_VALIDATED',
            },
            {
                kind: 'filesystem-signature',
                type: 'NTFS',
                label: '',
                offsetBytes: target.offsetBytes + 1048576,
                sizeBytes: 1048576,
                validation: 'SIGNATURE_ONLY',
            },
        ],
        mountedVolumes: [],
        formattedSizeBytes: size,
        unusedSuffixBytes: target.sizeBytes - size,
        partitionOptions: [{ partitionCount: 8, partitionSizeBytes: 1_073_740_800, unusedTailBytes: 3072 }],
        confirmationText: `ERASE /dev/synthetic${target.scsi ? ` SCSI ${target.scsi.id}` : ''}`,
    };
}
