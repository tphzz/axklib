import type { SamplerRelationship } from './transport';

export function isConfirmedRelationship(relationship: Pick<SamplerRelationship, 'quality'>): boolean {
    return relationship.quality === 'KNOWN';
}
