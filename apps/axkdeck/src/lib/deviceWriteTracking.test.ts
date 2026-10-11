import { beforeEach, describe, expect, it } from 'vitest';
import { DeviceWriteTracking } from './deviceWriteTracking';

describe('physical write identities', () => {
    beforeEach(() => localStorage.clear());
    const source = {
        kind: 'device' as const,
        reference: { sessionId: 'session', targetId: 'disk' },
        displayName: 'Test disk',
    };
    it('persists the identity before submission and blocks a blind retry after a lost acknowledgement', () => {
        const tracking = new DeviceWriteTracking(localStorage);
        tracking.bind('image', source);
        expect(tracking.begin('image', 'images.alter', 'known-id')).toBe('known-id');
        expect(new DeviceWriteTracking(localStorage).pending()[0]?.operationId).toBe('known-id');
        expect(() => tracking.begin('image', 'images.alter', 'another-id')).toThrow('uncertain outcome');
    });
    it('retains acknowledged jobs until their terminal status is actually retrieved', () => {
        const tracking = new DeviceWriteTracking(localStorage);
        tracking.bind('image', source);
        tracking.begin('image', 'images.alter', 'known-id');
        tracking.acknowledge('known-id', 'job');
        expect(new DeviceWriteTracking(localStorage).pending()[0]?.jobId).toBe('job');
        tracking.terminal('unrelated');
        expect(tracking.pending()).toHaveLength(1);
        tracking.terminal('job');
        expect(tracking.pending()).toHaveLength(0);
    });
    it('refuses device writes when durable client tracking is unavailable', () => {
        const tracking = new DeviceWriteTracking();
        tracking.bind('image', source);
        expect(() => tracking.begin('image', 'images.alter', 'known-id')).toThrow('unavailable');
        expect(tracking.begin('file-image', 'images.alter', 'file-id')).toBeUndefined();
    });
});
