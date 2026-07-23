import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import test from 'node:test';

import { normalizeTauriArguments, readBuildIdentity, tauriVersionConfig } from './build-version.mjs';

function fixture(metadata, packageBasename = 'axklib-feature-audio-a1b2c3d') {
    const directory = mkdtempSync(join(tmpdir(), 'axkdeck-version-'));
    const versionPath = join(directory, 'version.json');
    const packagePath = join(directory, 'package.txt');
    writeFileSync(versionPath, JSON.stringify(metadata));
    writeFileSync(packagePath, `${packageBasename}\n`);
    return readBuildIdentity(versionPath, packagePath);
}

function metadataFixture() {
    return {
        schema_version: 1,
        semantic_version: '0.0.0',
        project_version: '0.0.0',
        major: 0,
        minor: 0,
        patch: 0,
        release_tag: '',
        is_release: false,
        is_prerelease: false,
    };
}

test('branch builds use source identity for artifacts and 0.0.0 for Tauri', () => {
    const identity = fixture(metadataFixture());

    assert.equal(identity.artifactIdentity, 'feature-audio-a1b2c3d');
    assert.equal(identity.desktopPackageBasename, 'axkdeck-feature-audio-a1b2c3d');
    assert.deepEqual(tauriVersionConfig(identity, 'linux', '12'), { version: '0.0.0' });
});

test('native package basename accepts one LF- or CRLF-terminated line', () => {
    const directory = mkdtempSync(join(tmpdir(), 'axkdeck-version-newline-'));
    const versionPath = join(directory, 'version.json');
    const packagePath = join(directory, 'package.txt');
    writeFileSync(versionPath, JSON.stringify(metadataFixture()));
    writeFileSync(packagePath, 'axklib-feature-windows-a1b2c3d\r\n');

    assert.equal(readBuildIdentity(versionPath, packagePath).sourceIdentity, 'feature-windows-a1b2c3d');
});

test('pnpm argument separator is not forwarded to the Tauri CLI', () => {
    assert.deepEqual(normalizeTauriArguments(['--', '--no-bundle']), ['--no-bundle']);
    assert.deepEqual(normalizeTauriArguments(['--target', 'universal-apple-darwin']), [
        '--target',
        'universal-apple-darwin',
    ]);
});

test('tag builds use complete semantic version while macOS uses numeric bundle fields', () => {
    const identity = fixture(
        {
            schema_version: 1,
            semantic_version: '1.2.3-rc.4+build.9',
            project_version: '1.2.3',
            major: 1,
            minor: 2,
            patch: 3,
            release_tag: 'v1.2.3-rc.4+build.9',
            is_release: true,
            is_prerelease: true,
        },
        'axklib-v1.2.3-rc.4-build.9-a1b2c3d',
    );

    assert.equal(identity.artifactIdentity, '1.2.3-rc.4+build.9');
    assert.deepEqual(tauriVersionConfig(identity, 'win32', '42'), {
        version: '1.2.3-rc.4+build.9',
    });
    assert.deepEqual(tauriVersionConfig(identity, 'darwin', '42'), {
        version: '1.2.3',
        bundle: { macOS: { bundleVersion: '42' } },
    });
});

test('inconsistent version metadata is rejected', () => {
    assert.throws(
        () =>
            fixture({
                schema_version: 1,
                semantic_version: '1.2.3',
                project_version: '1.2.4',
                major: 1,
                minor: 2,
                patch: 3,
                release_tag: 'v1.2.3',
                is_release: true,
                is_prerelease: false,
            }),
        /project_version/,
    );
});

test('prerelease state and strict SemVer syntax are validated', () => {
    const base = {
        schema_version: 1,
        semantic_version: '1.2.3-rc.1',
        project_version: '1.2.3',
        major: 1,
        minor: 2,
        patch: 3,
        release_tag: 'v1.2.3-rc.1',
        is_release: true,
        is_prerelease: false,
    };
    assert.throws(() => fixture(base), /is_prerelease/);
    assert.throws(
        () =>
            fixture({
                ...base,
                semantic_version: '1.2.3-rc.01',
                release_tag: 'v1.2.3-rc.01',
                is_prerelease: true,
            }),
        /leading zero/,
    );
});
