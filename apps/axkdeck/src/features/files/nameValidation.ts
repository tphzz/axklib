import type { FilesystemRootCapabilities } from '../../lib/filesystem';

export function normalizeFilesystemName(name: string, capabilities: FilesystemRootCapabilities): string {
    return capabilities.namePolicy === 'FAT_8_3_UPPERCASE'
        ? name.replace(/[a-z]/g, (letter) => letter.toUpperCase())
        : name;
}

export function filesystemNameError(name: string, capabilities: FilesystemRootCapabilities): string | null {
    name = normalizeFilesystemName(name, capabilities);
    if (!name) return 'Enter a name.';
    if (name === '.' || name === '..' || /[/\\\0]/.test(name)) return 'Enter one name without path separators.';
    if (capabilities.namePolicy === 'FAT_8_3_UPPERCASE') {
        const [stem, extension, ...extra] = name.split('.');
        if (!stem) return 'The name before the extension must contain 1-8 characters.';
        if (stem.length > 8) return 'The name before the extension must be at most 8 characters.';
        if (extra.length || extension === '') return 'Use at most one dot followed by a 1-3 character extension.';
        if (extension && extension.length > 3) return 'The extension must be at most 3 characters.';
        if (!/^[A-Z0-9!#$%&'()@^_`{}~.-]+$/.test(name))
            return "Use ASCII letters, digits, or ! # $ % & ' ( ) - @ ^ _ ` { } ~. Spaces and other characters are unsupported.";
    }
    if (new TextEncoder().encode(name).length > capabilities.maximumNameBytes)
        return `The name must be at most ${capabilities.maximumNameBytes} bytes.`;
    try {
        if (capabilities.namePattern && new RegExp(capabilities.namePattern).exec(name)?.[0] === name) return null;
    } catch {
        return 'Naming rules are unavailable for this destination.';
    }
    return capabilities.nameHint || 'This name is unsupported by the destination filesystem.';
}

export function validFilesystemName(name: string, capabilities: FilesystemRootCapabilities): boolean {
    return filesystemNameError(name, capabilities) === null;
}
