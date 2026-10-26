import { fireEvent, waitFor } from '@testing-library/svelte';

export async function openSelect(control: HTMLElement): Promise<HTMLElement> {
    await waitFor(() => {
        if (control.matches(':disabled')) throw new Error('Selector is disabled');
    });
    if (control.getAttribute('aria-expanded') !== 'true') await fireEvent.click(control);
    return waitFor(() => {
        const popup = document.getElementById(control.getAttribute('aria-controls') ?? '');
        if (!popup) throw new Error('Missing selector popup');
        return popup;
    });
}

export async function selectValue(control: HTMLElement, value: string | number): Promise<void> {
    const popup = await openSelect(control);
    const option = await waitFor(() => {
        const option = [...popup.querySelectorAll<HTMLElement>('[role="option"]')].find(
            (item) => item.dataset.value === String(value),
        );
        if (!option) throw new Error(`Missing option ${value} in ${control.getAttribute('aria-label')}`);
        return option;
    });
    await fireEvent.click(option);
}
