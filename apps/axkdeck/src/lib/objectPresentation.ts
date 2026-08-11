import type { SamplerObject } from './transport';

export function objectPresentationName(object: SamplerObject, visibleNames: ReadonlyMap<string, string>): string {
    return object.objectType === 'SBAC' ? object.name : (visibleNames.get(object.key) ?? object.name);
}
