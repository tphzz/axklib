import type { ConversionTransport } from './conversions';
import type { AxklibHttpApiClient } from './httpApiClient';
import { HttpConversions } from './httpConversions';
import type { HttpImageSessions } from './httpImageSessions';
import type { HttpJobController } from './httpJobController';
import { HttpPackageOperations } from './httpPackageOperations';
import { HttpPackageTransport } from './httpPackageTransport';

export class HttpConversionTransport extends HttpPackageTransport implements ConversionTransport {
    private readonly conversions: HttpConversions;
    protected constructor(client: AxklibHttpApiClient, jobs: HttpJobController, sessions: HttpImageSessions) {
        super(sessions, new HttpPackageOperations(client, jobs, sessions));
        this.conversions = new HttpConversions(client, jobs, sessions);
    }
    resolveCompanionSources: ConversionTransport['resolveCompanionSources'] = (...args) =>
        this.conversions.resolveCompanionSources(...args);
    conversionProfiles: ConversionTransport['conversionProfiles'] = () => this.conversions.conversionProfiles();
    prepareConversion: ConversionTransport['prepareConversion'] = (...args) =>
        this.conversions.prepareConversion(...args);
    conversionPlan: ConversionTransport['conversionPlan'] = (...args) => this.conversions.conversionPlan(...args);
    releaseConversion: ConversionTransport['releaseConversion'] = (...args) =>
        this.conversions.releaseConversion(...args);
    executeConversion: ConversionTransport['executeConversion'] = (...args) =>
        this.conversions.executeConversion(...args);
    conversionExecution: ConversionTransport['conversionExecution'] = (...args) =>
        this.conversions.conversionExecution(...args);
}
