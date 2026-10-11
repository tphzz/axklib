import type { DeviceSession } from '../../lib/httpDevices';

type Target = DeviceSession['targets'][number];
const typeLabels = {
    FIXED: 'Hard disk',
    REMOVABLE: 'Removable disk',
    OPTICAL: 'CD-ROM',
    FLOPPY: 'Floppy',
    MAGNETO_OPTICAL: 'Magneto-optical',
    SEQUENTIAL: 'Sequential device',
};
export function deviceTargetType(target: Target): string {
    return target.scsi ? typeLabels[target.scsi.deviceType] : 'Disk';
}
export function deviceTargetLabel(target: Target): string {
    const id = target.scsi ? `SCSI ID ${target.scsi.id}` : target.label;
    return `${id} / ${deviceTargetType(target)} / ${(target.sizeBytes / 1e9).toFixed(2)} GB / [${target.offsetBytes}, ${target.offsetBytes + target.sizeBytes}) B${target.readOnly ? ' / read-only' : ''}`;
}
