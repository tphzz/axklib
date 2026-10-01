import type { CapacityAdmission, CapacityPolicy } from '../lib/importCapacity';

export const fitsCapacity: CapacityAdmission = {
    target: 'A3000',
    reports: [],
    allowed: true,
};
export async function inspectFitsCapacity(
    _session: number,
    _request: unknown,
    policy: CapacityPolicy,
): Promise<CapacityAdmission> {
    return { ...fitsCapacity, target: policy.target };
}
