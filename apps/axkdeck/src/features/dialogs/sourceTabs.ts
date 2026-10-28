export const sourceTabs = ['Image', 'Device', 'SCSI2SDv6', 'SCSI2SD XML'] as const;
export type SourceTab = (typeof sourceTabs)[number];
