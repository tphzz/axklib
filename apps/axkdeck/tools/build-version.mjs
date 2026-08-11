import { execFileSync, spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const semverPattern = /^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$/;
const packageBasenamePattern = /^axklib-[A-Za-z0-9._-]+$/;

function requireInteger(value, name) {
    if (!Number.isSafeInteger(value) || value < 0) {
        throw new Error(`${name} must be a non-negative integer`);
    }
    return value;
}

function requireString(value, name) {
    if (typeof value !== 'string') throw new Error(`${name} must be a string`);
    return value;
}

export function readBuildIdentity(versionMetadataPath, packageBasenamePath) {
    const metadata = JSON.parse(readFileSync(versionMetadataPath, 'utf8'));
    if (metadata.schema_version !== 1) throw new Error('unsupported version metadata schema');

    const semanticVersion = requireString(metadata.semantic_version, 'semantic_version');
    const projectVersion = requireString(metadata.project_version, 'project_version');
    const releaseTag = requireString(metadata.release_tag, 'release_tag');
    const semanticMatch = semanticVersion.match(semverPattern);
    if (!semanticMatch) throw new Error('semantic_version is not valid SemVer');
    if (
        semanticMatch[4]?.split('.').some((value) => /^[0-9]+$/.test(value) && value.length > 1 && value[0] === '0')
    ) {
        throw new Error('semantic_version has a numeric prerelease identifier with a leading zero');
    }

    const major = requireInteger(metadata.major, 'major');
    const minor = requireInteger(metadata.minor, 'minor');
    const patch = requireInteger(metadata.patch, 'patch');
    if (projectVersion !== `${major}.${minor}.${patch}`) {
        throw new Error('project_version does not match its numeric components');
    }
    if (typeof metadata.is_release !== 'boolean' || typeof metadata.is_prerelease !== 'boolean') {
        throw new Error('version metadata release flags must be boolean');
    }
    if (metadata.is_prerelease !== Boolean(semanticMatch[4])) {
        throw new Error('is_prerelease does not match semantic_version');
    }
    if (metadata.is_release) {
        if (releaseTag !== `v${semanticVersion}`) {
            throw new Error('release_tag does not match semantic_version');
        }
    } else if (semanticVersion !== '0.0.0' || releaseTag !== '') {
        throw new Error('development builds must use semantic version 0.0.0 without a release tag');
    }

    const packageText = readFileSync(packageBasenamePath, 'utf8');
    const packageMatch = packageText.match(/^([^\r\n]+)\r?\n$/);
    if (!packageMatch) {
        throw new Error('package basename must contain one newline-terminated line');
    }
    const nativePackageBasename = packageMatch[1];
    if (!packageBasenamePattern.test(nativePackageBasename)) {
        throw new Error('invalid axklib package basename');
    }
    const sourceIdentity = nativePackageBasename.slice('axklib-'.length);
    return {
        schemaVersion: 1,
        semanticVersion,
        projectVersion,
        releaseTag,
        isRelease: metadata.is_release,
        isPrerelease: metadata.is_prerelease,
        sourceIdentity,
        nativePackageBasename,
        desktopPackageBasename: `axkdeck-${sourceIdentity}`,
        artifactIdentity: metadata.is_release ? semanticVersion : sourceIdentity,
    };
}

export function tauriVersionConfig(identity, platform, buildNumber) {
    if (platform !== 'darwin') return { version: identity.semanticVersion };
    if (!/^[0-9]+$/.test(buildNumber)) throw new Error('macOS build number must be numeric');
    return {
        version: identity.projectVersion,
        bundle: { macOS: { bundleVersion: buildNumber } },
    };
}

export function normalizeTauriArguments(arguments_) {
    return arguments_[0] === '--' ? arguments_.slice(1) : arguments_;
}

function gitCommitCount(repositoryRoot) {
    try {
        return execFileSync('git', ['rev-list', '--count', 'HEAD'], {
            cwd: repositoryRoot,
            encoding: 'utf8',
            stdio: ['ignore', 'pipe', 'ignore'],
        }).trim();
    } catch {
        return '0';
    }
}

function main() {
    const [command, ...arguments_] = process.argv.slice(2);
    if (!['build', 'dev'].includes(command)) {
        throw new Error('usage: build-version.mjs <build|dev> [tauri arguments]');
    }

    const desktopRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
    const repositoryRoot = resolve(desktopRoot, '../..');
    const nativeBuildDirectory = resolve(
        process.env.AXKLIB_BUILD_DIRECTORY ?? resolve(repositoryRoot, 'build/native/release'),
    );
    const versionMetadataPath = resolve(
        process.env.AXKLIB_VERSION_METADATA_FILE ?? resolve(nativeBuildDirectory, 'version_metadata.json'),
    );
    const packageBasenamePath = resolve(
        process.env.AXKLIB_PACKAGE_BASENAME_FILE ?? resolve(nativeBuildDirectory, 'package_basename.txt'),
    );
    const identity = readBuildIdentity(versionMetadataPath, packageBasenamePath);
    const buildNumber = process.env.AXKDECK_BUILD_NUMBER ?? gitCommitCount(repositoryRoot);
    const config = tauriVersionConfig(identity, process.platform, buildNumber);
    const tauriCli = resolve(desktopRoot, 'node_modules/@tauri-apps/cli/tauri.js');
    const environment = {
        ...process.env,
        AXKLIB_BUILD_DIRECTORY: nativeBuildDirectory,
        AXKLIB_VERSION_METADATA_FILE: versionMetadataPath,
        AXKLIB_PACKAGE_BASENAME_FILE: packageBasenamePath,
        AXKDECK_SEMANTIC_VERSION: identity.semanticVersion,
        AXKDECK_PROJECT_VERSION: identity.projectVersion,
        AXKDECK_SOURCE_IDENTITY: identity.sourceIdentity,
        AXKDECK_RELEASE_TAG: identity.releaseTag,
        AXKDECK_IS_RELEASE: String(identity.isRelease),
    };
    const result = spawnSync(
        process.execPath,
        [tauriCli, command, '--config', JSON.stringify(config), ...normalizeTauriArguments(arguments_)],
        { cwd: desktopRoot, env: environment, stdio: 'inherit' },
    );
    if (result.error) throw result.error;
    process.exitCode = result.status ?? 1;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? '').href) main();
