import type { VolumeCapacityProfile } from './transport';

export function capacityStatus(status: VolumeCapacityProfile['status']): string {
    return status === 'FITS' ? 'Fits' : 'Does not fit';
}

export function capacityMetric(exact: number | null, minimum: number | null, limit: number, unit = ''): string {
    const scale = unit === 'B' ? 1024 : 1;
    const suffix = unit === 'B' ? ' KiB' : unit ? ` ${unit}` : '';
    const format = (value: number) => value.toLocaleString(undefined, { maximumFractionDigits: scale === 1 ? 0 : 1 });
    const maximum = `${format(limit / scale)}${suffix}`;
    if (exact !== null) return `${format(exact / scale)} / ${maximum}`;
    if (minimum !== null) {
        const lower = Math.floor((minimum / scale) * 10) / 10;
        return `\u2265 ${format(lower)} / ${maximum}`;
    }
    return `Not available / ${maximum}`;
}

export function capacityMemoryHelp(profile: VolumeCapacityProfile): string {
    const amount =
        profile.peakBytes !== null
            ? `Maximum needed while loading: ${profile.peakBytes.toLocaleString()} bytes.`
            : profile.minimumResidentBytes !== null
              ? `At least ${profile.minimumResidentBytes.toLocaleString()} bytes are required. The maximum during loading is unknown.`
              : 'The amount required during loading is unknown.';
    return `${amount} Limit: ${profile.parameterByteLimit.toLocaleString()} bytes.\nFresh power-on, Wipe, then full VOLUME/LOAD. Audio RAM and disk space are separate.`;
}

export function capacitySlotsHelp(profile: VolumeCapacityProfile): string {
    return `Samples, Sample Banks, Programs, Wave metadata and Sequences share these slots.\n${profile.peakSlots !== null ? `Maximum needed while loading: ${profile.peakSlots.toLocaleString()}.` : profile.minimumResidentSlots !== null ? `At least ${profile.minimumResidentSlots.toLocaleString()} slots are required; the loading maximum is unknown.` : 'The number required is unknown.'} Limit: ${profile.sharedObjectSlotLimit.toLocaleString()}.`;
}
