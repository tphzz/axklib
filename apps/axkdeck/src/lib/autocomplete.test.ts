import { describe, expect, it, vi } from 'vitest';

import { dismissAutocompleteFromOutsidePointer } from './autocomplete';

describe('dismissAutocompleteFromOutsidePointer', () => {
    it('dismisses only expanded autocompletes receiving a pointer event outside their wrapper', () => {
        const autocomplete = document.createElement('div');
        const option = document.createElement('button');
        const outside = document.createElement('button');
        autocomplete.append(option);
        document.body.append(autocomplete, outside);
        const ondismiss = vi.fn();

        const action = dismissAutocompleteFromOutsidePointer(autocomplete, { expanded: true, ondismiss });

        option.dispatchEvent(new Event('pointerdown', { bubbles: true, composed: true }));
        expect(ondismiss).not.toHaveBeenCalled();

        outside.dispatchEvent(new Event('pointerdown', { bubbles: true, composed: true }));
        expect(ondismiss).toHaveBeenCalledOnce();

        action.update({ expanded: false, ondismiss });
        outside.dispatchEvent(new Event('pointerdown', { bubbles: true, composed: true }));
        expect(ondismiss).toHaveBeenCalledOnce();

        action.update({ expanded: true, ondismiss });
        action.destroy();
        outside.dispatchEvent(new Event('pointerdown', { bubbles: true, composed: true }));
        expect(ondismiss).toHaveBeenCalledOnce();
        autocomplete.remove();
        outside.remove();
    });
});
