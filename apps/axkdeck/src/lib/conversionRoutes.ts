import type { ConversionProfile } from './conversions';

const supportedPanels = new Set([
    'disk-set-selection',
    'a-series-volume-placement',
    'a-series-load-target',
    'ordered-wave-selection',
    'new-image-file',
]);

interface RouteQuery {
    sourceFormat: string;
    destinationFormat?: string;
    outputKind?: ConversionProfile['outputKind'];
    medium?: string;
    intent?: string;
    editorPanels?: readonly string[];
}

export function conversionRoutes(profiles: readonly ConversionProfile[], query: RouteQuery) {
    return profiles
        .flatMap((profile) => {
            const { route } = profile;
            if (
                !route ||
                profile.sourceFormat !== query.sourceFormat ||
                (query.destinationFormat !== undefined && profile.destinationFormat !== query.destinationFormat) ||
                (query.outputKind !== undefined && profile.outputKind !== query.outputKind) ||
                (query.medium !== undefined && route.medium !== query.medium) ||
                (query.intent !== undefined && !route.intents.includes(query.intent)) ||
                (query.editorPanels !== undefined &&
                    (query.editorPanels.length !== route.editorCapabilities.length ||
                        !query.editorPanels.every((panel) => route.editorCapabilities.includes(panel))))
            )
                return [];
            const missingCapabilities = route.editorCapabilities.filter(
                (capability) => !supportedPanels.has(capability),
            );
            return [{ profile, route, supported: missingCapabilities.length === 0, missingCapabilities }];
        })
        .sort(
            (a, b) =>
                a.route.priority - b.route.priority ||
                (a.profile.id < b.profile.id ? -1 : a.profile.id > b.profile.id ? 1 : 0),
        );
}

/** Dialogs without a route chooser must not silently break a registry priority tie. */
export function preferredConversionRoute(matches: ReturnType<typeof conversionRoutes>) {
    const [first, next] = matches.filter((match) => match.supported);
    if (first && next && first.route.priority === next.route.priority)
        throw new Error('More than one compatible converter has the same priority. No converter was selected.');
    return first;
}
