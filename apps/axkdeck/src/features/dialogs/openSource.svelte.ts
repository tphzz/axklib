import type { DeviceCandidate, DeviceService, DeviceSession, DeviceXmlInspection } from '../../lib/httpDevices';
import type { ImageLocation } from '../../lib/storageLocations';
import type { ImageSessionPreparation } from '../image-session/actions';
import { userFacingMessage } from '../../lib/userFacingMessage';
import { AxklibApiError } from '../../lib/httpErrors';
import type { SourceTab } from './sourceTabs';

type XmlFile = Pick<File, 'name' | 'size' | 'text'>;
type DeviceAccess = 'read-only' | 'read-write';

export class OpenSourceWorkflow {
    tab = $state<SourceTab>('Image');
    available = $state(false);
    unavailableReason = $state('Device access requires the local desktop backend.');
    busy = $state(false);
    listing = $state(false);
    validating = $state(false);
    readingConfiguration = $state(false);
    status = $state('');
    candidates = $state<DeviceCandidate[]>([]);
    private deviceId = $state('');
    private xmlDeviceId = $state('');
    private xmlSelectedTargetId = $state('');
    private v6DeviceId = $state('');
    private v6SelectedTargetId = $state('');
    private configurationReady = $state(false);
    private requestedAccess = $state<DeviceAccess>('read-write');
    private writesSupported = $state(false);
    acknowledged = $state(false);
    xmlName = $state('');
    xml = $state('');
    xmlTargets = $state<DeviceXmlInspection['targets']>([]);
    connection = $state<DeviceSession | null>(null);
    private listed = $state(false);
    private disposed = false;
    private xmlGeneration = 0;
    private listGeneration = 0;
    private configurationGeneration = 0;
    private configurationTask: Promise<void> = Promise.resolve();

    constructor(
        private readonly service: DeviceService | undefined,
        private readonly openImage: (location: ImageLocation, prepare?: ImageSessionPreparation) => Promise<boolean>,
        private readonly cleanupImage: () => Promise<void> = async () => {},
    ) {}

    get candidateId() {
        return this.deviceId;
    }
    set candidateId(value: string) {
        if (value !== this.deviceId) this.acknowledged = false;
        this.deviceId = value;
    }
    get xmlCandidateId() {
        return this.xmlDeviceId;
    }
    set xmlCandidateId(value: string) {
        if (value !== this.xmlDeviceId) this.acknowledged = false;
        this.xmlDeviceId = value;
    }
    get xmlTargetId() {
        return this.xmlSelectedTargetId;
    }
    set xmlTargetId(value: string) {
        if (value !== this.xmlSelectedTargetId) this.acknowledged = false;
        this.xmlSelectedTargetId = value;
    }
    get v6CandidateId() {
        return this.v6DeviceId;
    }
    get v6TargetId() {
        return this.v6SelectedTargetId;
    }
    set v6TargetId(value: string) {
        if (value !== this.v6SelectedTargetId) this.acknowledged = false;
        this.v6SelectedTargetId = value;
    }
    get v6Targets() {
        return this.configurationReady ? (this.connection?.targets ?? []) : [];
    }
    get connectionBlocksControls() {
        return !!this.connection && this.tab !== 'SCSI2SDv6';
    }
    get writeUnavailableReason() {
        if (!this.writesSupported) return 'Writing is unavailable on this platform.';
        if (this.selectedCandidate && this.selectedCandidate.sectorSize !== 512) return 'This target is read-only.';
        if (this.tab === 'SCSI2SD XML' && this.selectedTarget?.readOnly) return 'This configured target is read-only.';
        if (this.tab === 'SCSI2SDv6' && this.selectedTarget?.readOnly) return 'This configured target is read-only.';
        return '';
    }
    get access(): DeviceAccess {
        return this.writeUnavailableReason ? 'read-only' : this.requestedAccess;
    }
    set access(value: DeviceAccess) {
        if (value !== this.requestedAccess) this.acknowledged = false;
        this.requestedAccess = value;
    }
    get visibleCandidates() {
        return this.candidates.filter((candidate) =>
            this.tab === 'SCSI2SD XML' || this.tab === 'SCSI2SDv6'
                ? candidate.sectorSize === 512
                : candidate.sizeBytes > 0,
        );
    }
    get selectedCandidate() {
        const id =
            this.tab === 'SCSI2SD XML'
                ? this.xmlCandidateId
                : this.tab === 'SCSI2SDv6'
                  ? this.v6CandidateId
                  : this.candidateId;
        return this.visibleCandidates.find((candidate) => candidate.id === id);
    }
    get selectedTarget() {
        if (this.tab === 'SCSI2SDv6') return this.v6Targets.find((target) => target.id === this.v6TargetId);
        return this.xmlTargets.find((target) => target.id === this.xmlTargetId && target.enabled && target.eligible);
    }
    get canOpen() {
        return (
            this.available &&
            this.listed &&
            !this.busy &&
            !this.listing &&
            !this.validating &&
            !this.readingConfiguration &&
            (!this.connection || (this.tab === 'SCSI2SDv6' && this.configurationReady)) &&
            !!this.selectedCandidate &&
            (this.access === 'read-only' || this.acknowledged) &&
            (this.tab === 'Device' ||
                (this.tab === 'SCSI2SD XML' && !!this.xml && !!this.selectedTarget) ||
                (this.tab === 'SCSI2SDv6' && this.configurationReady && !!this.selectedTarget))
        );
    }
    async initialize() {
        if (!this.service) return;
        try {
            const available = await this.service.available();
            if (this.disposed) return;
            this.available = available;
            this.unavailableReason = available ? '' : 'This local backend does not include device access.';
        } catch (error) {
            if (!this.disposed) this.unavailableReason = userFacingMessage(error);
        }
    }
    async activate(tab: SourceTab) {
        if (this.disposed || this.busy || (tab !== 'Image' && !this.available)) return;
        if (this.tab !== tab && (this.connection || this.readingConfiguration)) {
            this.busy = true;
            ++this.configurationGeneration;
            await this.configurationTask;
            const released = await this.release();
            this.busy = false;
            this.readingConfiguration = false;
            if (!released || this.disposed) return;
        }
        if (this.tab !== tab) this.acknowledged = false;
        this.tab = tab;
        this.status = '';
        if (!this.listed && (tab === 'Device' || tab === 'SCSI2SDv6' || (tab === 'SCSI2SD XML' && this.xml)))
            await this.refresh();
        else if (tab === 'SCSI2SDv6' && this.v6CandidateId && !this.configurationReady)
            await this.selectV6Device(this.v6CandidateId);
    }
    async refresh() {
        if (
            this.disposed ||
            !this.service ||
            !this.available ||
            this.busy ||
            (this.connection && this.tab !== 'SCSI2SDv6') ||
            this.readingConfiguration ||
            this.tab === 'Image' ||
            (this.tab === 'SCSI2SD XML' && !this.xml)
        )
            return;
        const generation = ++this.listGeneration;
        this.listing = true;
        this.listed = false;
        this.status = 'Reading USB device list';
        try {
            const result = await this.service.list();
            if (this.disposed || generation !== this.listGeneration) return;
            if (this.writesSupported !== result.writesSupported) this.acknowledged = false;
            this.writesSupported = result.writesSupported;
            this.candidates = result.devices;
            if (!result.devices.some((value) => value.id === this.candidateId)) this.candidateId = '';
            if (!result.devices.some((value) => value.id === this.xmlCandidateId && value.sectorSize === 512))
                this.xmlCandidateId = '';
            if (
                this.v6CandidateId &&
                !result.devices.some((value) => value.id === this.v6CandidateId && value.sectorSize === 512)
            )
                await this.selectV6Device('');
            this.listed = true;
            this.status = this.visibleCandidates.length
                ? 'Select a device and access mode.'
                : 'No eligible USB devices found.';
            if (this.tab === 'SCSI2SDv6' && this.v6CandidateId) {
                if (this.connection && this.configurationReady) {
                    const sessionId = this.connection.sessionId;
                    const current = await this.service.status(sessionId);
                    if (this.disposed || generation !== this.listGeneration || this.connection?.sessionId !== sessionId)
                        return;
                    this.connection = current;
                    if (this.connection.invalidated) {
                        this.v6TargetId = '';
                        this.acknowledged = false;
                        await this.release();
                        this.status = 'Device configuration changed. Select the device again.';
                    } else if (!this.v6Targets.some((value) => value.id === this.v6TargetId)) this.v6TargetId = '';
                } else await this.selectV6Device(this.v6CandidateId);
            }
        } catch (error) {
            if (!this.disposed && generation === this.listGeneration) {
                this.listed = false;
                this.status = userFacingMessage(error);
                if (this.tab === 'SCSI2SDv6' && this.connection) {
                    this.acknowledged = false;
                    await this.release();
                }
            }
        } finally {
            if (generation === this.listGeneration) this.listing = false;
        }
    }
    async selectV6Device(id: string): Promise<void> {
        if (this.disposed || this.busy || !this.service || this.tab !== 'SCSI2SDv6') return;
        this.v6DeviceId = id;
        this.v6TargetId = '';
        this.acknowledged = false;
        this.configurationReady = false;
        const generation = ++this.configurationGeneration;
        const candidate = this.visibleCandidates.find((value) => value.id === id);
        this.readingConfiguration = !!candidate;
        this.status = candidate ? 'Reading SCSI2SDv6 configuration' : 'Select a device.';
        // Serialize owned previews; a late permission response still needs explicit cleanup.
        this.configurationTask = this.configurationTask
            .then(async () => {
                if (this.disposed || generation !== this.configurationGeneration) return;
                if (!(await this.release()) || !candidate) return;
                try {
                    this.connection = await this.service!.connect(candidate.id, 'SCSI2SD_V6');
                    if (this.disposed || generation !== this.configurationGeneration) {
                        await this.release();
                        return;
                    }
                    if (this.connection.invalidated) throw new Error('Device configuration changed. Select it again.');
                    this.configurationReady = true;
                    this.status = this.connection.targets.length
                        ? 'Select a configured SCSI target.'
                        : 'No eligible SCSI targets found.';
                } catch (error) {
                    if (!this.disposed && generation === this.configurationGeneration)
                        this.status = userFacingMessage(error);
                    await this.release();
                }
            })
            .finally(() => {
                if (generation === this.configurationGeneration) this.readingConfiguration = false;
            });
        await this.configurationTask;
    }
    async loadXml(file: XmlFile) {
        if (this.disposed || this.busy || this.connection || !this.service || !this.available) return;
        this.acknowledged = false;
        const generation = ++this.xmlGeneration;
        this.xmlName = file.name;
        this.xml = '';
        this.xmlTargets = [];
        this.xmlTargetId = '';
        this.xmlCandidateId = '';
        this.validating = true;
        this.status = 'Validating SCSI2SD XML';
        try {
            if (file.size > 262144) throw new Error('Configuration XML exceeds 256 KiB.');
            const xml = await file.text();
            if (this.disposed || generation !== this.xmlGeneration) return;
            const inspection = await this.service.inspectXml(xml);
            if (this.disposed || generation !== this.xmlGeneration) return;
            this.xml = xml;
            this.xmlTargets = inspection.targets;
            this.status = inspection.targets.some((target) => target.enabled && target.eligible)
                ? 'XML validated. Select a device and configured target.'
                : 'XML validated; no eligible targets are enabled.';
            if (this.tab === 'SCSI2SD XML' && !this.listed) await this.refresh();
        } catch (error) {
            if (!this.disposed && generation === this.xmlGeneration) this.status = userFacingMessage(error);
        } finally {
            if (generation === this.xmlGeneration) this.validating = false;
        }
    }
    async open(): Promise<boolean> {
        if (!this.canOpen || !this.service || !this.selectedCandidate) return false;
        const candidate = this.selectedCandidate;
        const xmlMode = this.tab === 'SCSI2SD XML';
        const v6Mode = this.tab === 'SCSI2SDv6';
        const targetId = xmlMode ? this.xmlTargetId : v6Mode ? this.v6TargetId : 'disk';
        const writeAccess = this.access === 'read-write';
        this.busy = true;
        this.status = 'Connecting read-only';
        let transferred = false;
        try {
            this.connection =
                v6Mode && this.connection
                    ? await this.service.status(this.connection.sessionId)
                    : await this.service.connect(
                          candidate.id,
                          xmlMode ? 'SCSI2SD_XML' : 'RAW',
                          xmlMode ? this.xml : undefined,
                      );
            if (this.disposed) return false;
            const target = this.connection.targets.find((value) => value.id === targetId);
            if (!target || this.connection.invalidated)
                throw new Error('The selected target is unavailable on this device.');
            if (writeAccess && (!this.connection.writesSupported || target.readOnly))
                throw new Error('This target is read-only. Select Read only to open it.');
            const sessionId = this.connection.sessionId;
            const prepare: ImageSessionPreparation | undefined = writeAccess
                ? async (image, onProgress) => {
                      if (this.disposed) throw new Error('Device opening was cancelled.');
                      if (image.format !== 'sfs')
                          throw new Error('This format is read-only. Select Read only to open it.');
                      this.status = 'Verifying device metadata';
                      await this.service!.authorize(sessionId, targetId, (progress) => {
                          this.status = progress.label;
                          onProgress?.(progress);
                      });
                  }
                : undefined;
            this.status = 'Opening device';
            transferred = await this.openImage(
                {
                    kind: 'device',
                    reference: { sessionId, targetId },
                    displayName: `${candidate.model} (${candidate.serial}) / ${target.label}`,
                },
                prepare,
            );
            if (!transferred) throw new Error('Device opening was cancelled or failed.');
            this.connection = null;
            this.configurationReady = false;
            return true;
        } catch (error) {
            if (!this.disposed) this.status = userFacingMessage(error);
            return false;
        } finally {
            if (!transferred) await this.release();
            this.busy = false;
        }
    }
    async release(): Promise<boolean> {
        this.configurationReady = false;
        if (!this.connection || !this.service) return true;
        try {
            await this.cleanupImage();
            try {
                await this.service.close(this.connection.sessionId);
            } catch (error) {
                if (!(error instanceof AxklibApiError) || error.code !== 'device_session_missing') throw error;
            }
            this.connection = null;
            this.configurationReady = false;
            return true;
        } catch (error) {
            this.status = `Connection cleanup failed: ${userFacingMessage(error)}`;
            return false;
        }
    }
    async cancel(): Promise<boolean> {
        if (this.busy) return false;
        this.busy = true;
        ++this.configurationGeneration;
        await this.configurationTask;
        this.readingConfiguration = false;
        const released = await this.release();
        this.busy = false;
        if (released) await this.dispose();
        return released;
    }
    async dispose() {
        this.disposed = true;
        ++this.xmlGeneration;
        ++this.listGeneration;
        ++this.configurationGeneration;
        await this.configurationTask;
        if (!this.busy) await this.release();
    }
}
