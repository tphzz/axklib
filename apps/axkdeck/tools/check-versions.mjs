import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const packageVersion = JSON.parse(readFileSync(resolve(root, 'package.json'), 'utf8')).version;
const tauri = JSON.parse(readFileSync(resolve(root, 'src-tauri/tauri.conf.json'), 'utf8'));
const sbom = JSON.parse(readFileSync(resolve(root, 'src-tauri/resources/axkdeck.spdx.json'), 'utf8'));
const cargo = readFileSync(resolve(root, 'src-tauri/Cargo.toml'), 'utf8');
const cargoVersion = cargo.match(/^version\s*=\s*"([^"]+)"/m)?.[1];

if (!cargoVersion) throw new Error('src-tauri/Cargo.toml package version is missing');
if (packageVersion !== '0.0.0' || cargoVersion !== '0.0.0') {
  throw new Error(
    `axkdeck private package placeholders must be 0.0.0: package.json=${packageVersion}, Cargo.toml=${cargoVersion}`,
  );
}
if (Object.hasOwn(tauri, 'version')) {
  throw new Error('tauri.conf.json must not contain a product version; the native build supplies it');
}
const desktopPackages = sbom.packages?.filter((value) => value.name === 'axkdeck') ?? [];
if (sbom.name !== 'axkdeck-release' || desktopPackages.length !== 1 || desktopPackages[0].versionInfo !== '0.0.0') {
  throw new Error('the checked-in desktop SBOM must contain one neutral axkdeck 0.0.0 product row');
}

process.stdout.write('axkdeck product version is supplied by the native build\n');
