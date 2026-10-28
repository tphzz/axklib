import type { DeviceService, DeviceCandidate, DeviceSession, DeviceLayout } from '../../lib/httpDevices';
import type { SourceTab } from '../dialogs/sourceTabs';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { AxklibApiError } from '../../lib/httpErrors';
import type { DeviceInspection } from '../../lib/httpDeviceFormatting';

export class CreateDeviceSelection {
    tab = $state<SourceTab>('Image');
    available = $state(false);
    reason = $state('Device formatting requires the local desktop backend.');
    candidates = $state<DeviceCandidate[]>([]);
    candidateId = $state('');
    targetId = $state('');
    connection = $state<DeviceSession | null>(null);
    xml = $state('');
    xmlName = $state('');
    busy = $state(false);
    status = $state('');
    constructor(
        readonly service: DeviceService | undefined,
        private readonly invalidate: () => void,
    ) {}
    get target() {
        return this.connection?.targets.find((target) => target.id === this.targetId);
    }
    get visibleCandidates() {
        return this.candidates.filter((candidate) => this.tab === 'Device' || candidate.sectorSize === 512);
    }
    async initialize() {
        try {
            this.available =
                !!this.service?.formatting &&
                (await this.service.available()) &&
                (await this.service.formatting.available());
            this.reason = this.available ? '' : 'This backend does not provide safe device formatting.';
        } catch (error) {
            this.reason = userFacingMessage(error);
        }
    }
    async activate(tab: SourceTab) {
        if (this.busy || (tab !== 'Image' && !this.available) || tab === this.tab) return;
        await this.run(async () => {
            if (!(await this.release())) return;
            this.invalidate();
            this.tab = tab;
            this.candidateId = '';
            this.candidates =
                tab === 'Image' || (tab === 'SCSI2SD XML' && !this.xml) ? [] : (await this.service!.list()).devices;
        });
    }
    async refresh() {
        if (this.tab === 'SCSI2SD XML' && !this.xml) return;
        await this.run(async () => {
            if (!(await this.release())) return;
            this.invalidate();
            this.candidateId = '';
            this.candidates = (await this.service!.list()).devices;
        });
    }
    async select(candidateId: string) {
        if (!this.visibleCandidates.some((candidate) => candidate.id === candidateId) && candidateId !== '') return;
        await this.run(async () => {
            if (!(await this.release())) return;
            this.invalidate();
            this.candidateId = candidateId;
            if (!candidateId) return;
            const layout: DeviceLayout =
                this.tab === 'Device' ? 'RAW' : this.tab === 'SCSI2SDv6' ? 'SCSI2SD_V6' : 'SCSI2SD_XML';
            this.connection = await this.service!.connect(candidateId, layout, this.xml);
            if (layout === 'RAW') this.targetId = this.connection.targets[0]?.id ?? '';
        });
    }
    async loadXml(file: Pick<File, 'name' | 'size' | 'text'>) {
        await this.run(async () => {
            if (!(await this.release())) return;
            this.invalidate();
            this.candidateId = '';
            this.xml = '';
            this.xmlName = '';
            if (file.size > 262144) throw new Error('Configuration XML exceeds 256 KiB.');
            const xml = await file.text();
            const inspected = await this.service!.inspectXml(xml);
            if (!inspected.targets.some((target) => target.enabled && target.eligible))
                throw new Error('No supported configured target is available.');
            this.xml = xml;
            this.xmlName = file.name;
            this.candidates = (await this.service!.list()).devices;
        });
    }
    async release(): Promise<boolean> {
        if (!this.connection) return true;
        try {
            await this.service!.close(this.connection.sessionId);
            this.connection = null;
            this.targetId = '';
            return true;
        } catch (error) {
            if (error instanceof AxklibApiError && error.code === 'device_session_missing') {
                this.connection = null;
                this.targetId = '';
                return true;
            }
            this.status = userFacingMessage(error);
            return false;
        }
    }
    async reconnect(inspection: DeviceInspection): Promise<void> {
        if (!(await this.release()))
            throw new Error(this.status || 'The previous device connection could not be released');
        const candidate = inspection.candidate;
        const listed = await this.service!.list();
        if (
            !listed.devices.some(
                (item) =>
                    item.id === candidate.id &&
                    item.sizeBytes === candidate.sizeBytes &&
                    item.model === candidate.model &&
                    item.serial === candidate.serial &&
                    item.identity.devicePath === candidate.identity.devicePath,
            )
        )
            throw new Error(
                'Formatting is complete. The selected device is no longer available; close Create and select it again through Open.',
            );
        const layout: DeviceLayout =
            this.tab === 'Device' ? 'RAW' : this.tab === 'SCSI2SDv6' ? 'SCSI2SD_V6' : 'SCSI2SD_XML';
        this.connection = await this.service!.connect(candidate.id, layout, this.xml);
        const target = this.connection.targets.find(
            (item) =>
                item.id === inspection.target.id &&
                item.offsetBytes === inspection.target.offsetBytes &&
                item.sizeBytes === inspection.target.sizeBytes &&
                item.sectorSize === inspection.target.sectorSize,
        );
        if (!target) {
            await this.release();
            throw new Error(
                'Formatting is complete. The target layout changed; close Create and select it again through Open.',
            );
        }
        this.targetId = target.id;
    }
    private async run(action: () => Promise<void>) {
        if (this.busy || !this.service) return;
        this.busy = true;
        this.status = '';
        try {
            await action();
        } catch (error) {
            this.status = userFacingMessage(error);
        } finally {
            this.busy = false;
        }
    }
}
