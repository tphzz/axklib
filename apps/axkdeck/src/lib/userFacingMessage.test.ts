import { describe, expect, it } from 'vitest';

import { userFacingMessage } from './userFacingMessage';

describe('userFacingMessage', () => {
    it('sentence-cases lower-case service errors', () => {
        expect(userFacingMessage(new Error('directory could not be created'))).toBe('Directory could not be created');
    });

    it('preserves existing capitalization and lowercase product names', () => {
        expect(userFacingMessage(new Error('Destination already exists'))).toBe('Destination already exists');
        expect(userFacingMessage(new Error('axklib-server returned an invalid response'))).toBe(
            'axklib-server returned an invalid response',
        );
    });
});
