#!/usr/bin/env node
// Writes the Homebrew Channel manifest and a one-app repository index for a release.
//   node scripts/hbc-repo.js <tag> <ipk-path>
// Outputs webosbrew.manifest.json and apps.json next to the IPK. Add
//   https://github.com/daredoes/projectm-webos/releases/latest/download/apps.json
// as a custom repository in Homebrew Channel to install and update from releases.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const REPO = 'https://github.com/daredoes/projectm-webos';
const ICON = 'https://raw.githubusercontent.com/daredoes/projectm-webos/main/assets/icon.png';

const [tag, ipk] = process.argv.slice(2);
if (!tag || !ipk) {
  console.error('usage: node scripts/hbc-repo.js <tag> <ipk-path>');
  process.exit(1);
}
const appinfo = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'appinfo.json'), 'utf8'));
const base = `${REPO}/releases/download/${tag}/`;
const manifest = {
  id: appinfo.id,
  version: appinfo.version,
  type: appinfo.type,
  title: appinfo.title,
  appDescription: 'projectM (MilkDrop-compatible) music visualizer for whatever the TV plays',
  iconUri: ICON,
  sourceUrl: REPO,
  rootRequired: false,
  ipkUrl: base + path.basename(ipk),
  ipkHash: { sha256: crypto.createHash('sha256').update(fs.readFileSync(ipk)).digest('hex') },
};
const apps = {
  paging: { page: 1, count: 1, maxPage: 1, itemsTotal: 1, prevUrl: null, nextUrl: null },
  packages: [{
    id: appinfo.id,
    title: appinfo.title,
    iconUri: ICON,
    manifestUrl: base + 'webosbrew.manifest.json',
    manifest,
    category: 'multimedia',
    description: manifest.appDescription,
    pool: 'main',
  }],
};
const out = path.dirname(ipk);
fs.writeFileSync(path.join(out, 'webosbrew.manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
fs.writeFileSync(path.join(out, 'apps.json'), JSON.stringify(apps, null, 2) + '\n');
