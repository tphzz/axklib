import type { VolumeCapacityProfile, VolumeCapacityReport } from '../lib/volumeInspections';

export function volumeCapacityFixture(
    status: VolumeCapacityProfile['status'] = 'FITS',
    laterStatus = status,
): VolumeCapacityReport {
    return {
        baseline: 'FRESH_POWER_ON_WIPE_VOLUME_LOAD',
        partitionIndex: 0,
        volumeDirectoryId: 9,
        volumeName: 'Capacity Test',
        objectCounts: [{ type: 'SBNK', count: 40 }],
        profiles: (['A3000', 'A4000_A5000'] as const).map((target, index) => {
            const state = index === 0 ? status : laterStatus;
            return {
                target,
                status: state,
                parameterByteLimit: target === 'A3000' ? 524288 : 786432,
                sharedObjectSlotLimit: target === 'A3000' ? 1024 : 2048,
                baselineBytes: target === 'A3000' ? 87720 : 111280,
                baselineSlots: target === 'A3000' ? 129 : 130,
                minimumResidentBytes: state === 'DOES_NOT_FIT' ? 860704 : 123456,
                minimumResidentSlots: 419,
                residentBytes: state === 'FITS' ? 123456 : null,
                peakBytes: state === 'FITS' ? 124000 : null,
                peakSlots: state === 'FITS' ? 419 : null,
                reasons:
                    state === 'FITS'
                        ? []
                        : [
                              {
                                  code: 'RESIDENT_MINIMUM_EXCEEDS_POOL',
                                  message: 'The proven resident minimum exceeds the sampler parameter-memory pool.',
                              },
                          ],
            };
        }),
    };
}
