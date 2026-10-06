import type { ObjectDetail } from '../lib/transport';
import { sampleFormatFixture } from './sampleFormatFixture';

export function mappingDetail(id: string, revision = 1): ObjectDetail {
    const bank = id === 'bank';
    return {
        image: { revision },
        object: { id, key: id, type: bank ? 'SBAC' : 'SBNK', name: id },
        relationships: bank
            ? ['a', 'b', 'a'].map((member, index) => ({
                  id: `slot-${index}`,
                  type: 'SBAC_SLOT_TO_SBNK',
                  quality: 'KNOWN',
                  selectedObjectRoles: ['SOURCE'],
                  sourceObject: { id: 'bank', type: 'SBAC' },
                  targetObject: { id: member, type: 'SBNK' },
              }))
            : [],
        editing: {
            ...sampleFormatFixture(),
            profile: bank ? 'a-series/sample-bank' : 'a-series/sample',
            editable: true,
            reason: '',
            payloadSha256: id.padEnd(64, '0'),
            partitionIndex: 0,
            volumeName: 'Volume',
            parameters: { level: 100, key_low: 0, key_high: 127, root_key: 60, velocity_low: 0, velocity_high: 127 },
            playbackWindow: { start_frame: 0, length_frames: 100 },
            maximumFrames: 100,
            canEditPlayback: !bank,
            blockedParameters: [],
            blockedParameterReasons: {},
            unavailableParameters: {},
            sources: [],
            ...(bank
                ? {
                      bankOverrides: {
                          members: ['a', 'b', 'a'].map((id) => ({ objectId: id, name: id })),
                          units: [
                              { id: 33, keys: ['level'], selectors: [33], activeSelectors: [] },
                              { id: 37, keys: ['velocity_high'], selectors: [37], activeSelectors: [] },
                              { id: 38, keys: ['velocity_low'], selectors: [38], activeSelectors: [] },
                          ],
                      },
                  }
                : {}),
        },
    } as unknown as ObjectDetail;
}
