import type {
    ObjectFormatConversionRequest,
    ObjectFormatConversionSnapshot,
    ObjectStorageFormat,
} from '../../lib/objectEditing';
import type { ObjectDetail } from '../../lib/transport';

export function conversionFormat(snapshot?: ObjectFormatConversionSnapshot | null): ObjectStorageFormat | undefined {
    return snapshot?.kind === 'PROGRAM' ? snapshot.programFormat.format : snapshot?.sampleFormat.format;
}

export function sameConversionIdentity(
    a?: ObjectFormatConversionSnapshot | null,
    b?: ObjectFormatConversionSnapshot | null,
): boolean {
    if (!a || !b) return a === b;
    return (
        a.kind === b.kind &&
        a.partitionIndex === b.partitionIndex &&
        a.volumeName === b.volumeName &&
        (a.kind !== 'PROGRAM' ||
            (b.kind === 'PROGRAM' && a.programNumber === b.programNumber && a.programName === b.programName))
    );
}

export function conversionRequest(
    detail: ObjectDetail,
    target: ObjectStorageFormat,
): ObjectFormatConversionRequest | null {
    const snapshot = detail.formatConversion;
    if (!snapshot) return null;
    const common = {
        id: 'object-format',
        partition_index: snapshot.partitionIndex,
        volume_name: snapshot.volumeName,
        expected_payload_sha256: snapshot.payloadSha256,
    };
    if (snapshot.kind === 'PROGRAM') {
        if (
            detail.object.type !== 'PROG' ||
            snapshot.programNumber === null ||
            Number(detail.object.name) !== snapshot.programNumber ||
            (target !== 'A3000' && target !== 'A4000_A5000')
        )
            return null;
        return {
            expectedRevision: detail.image.revision,
            operation: {
                ...common,
                type: 'convert_prog_format',
                program_number: snapshot.programNumber,
                target_format: target === 'A3000' ? 'a3000' : 'a4000_a5000',
            },
        };
    }
    if (target !== 'A3000_188' && target !== 'A4000_A5000_224') return null;
    if ((snapshot.kind === 'SAMPLE_BANK' ? 'SBAC' : 'SBNK') !== detail.object.type) return null;
    return {
        expectedRevision: detail.image.revision,
        operation: {
            ...common,
            target_format: target === 'A3000_188' ? 'a3000_188' : 'a4000_a5000_224',
            ...(snapshot.kind === 'SAMPLE_BANK'
                ? { type: 'convert_sbac_format', sample_bank_name: detail.object.name }
                : { type: 'convert_sbnk_format', sample_name: detail.object.name }),
        },
    };
}
