//
// Platform binary resolution for the npm channel.
//
// Design note: the platform binaries are real npm packages listed in
// optionalDependencies with "os"/"cpu" fields, so npm itself downloads only the
// matching one. This is preferred over a postinstall that curls from GitHub
// because it works behind a corporate npm proxy, respects `npm ci
// --offline`, and keeps the integrity hash in the lockfile. The postinstall
// below therefore only *verifies* and sets the exec bit — it never downloads.
//
'use strict';

const fs = require('fs');
const path = require('path');

// npm's process.platform/arch spellings differ from our release triples.
const PLATFORM_MAP = { darwin: 'darwin', linux: 'linux' };
const ARCH_MAP = { arm64: 'arm64', x64: 'x64' };

function targetTriple() {
  const os = PLATFORM_MAP[process.platform];
  const arch = ARCH_MAP[process.arch];
  if (!os || !arch) {
    throw new Error(
      `unsupported platform: ${process.platform}-${process.arch}\n` +
      `  sparx publishes darwin-arm64, darwin-x64, linux-arm64, linux-x64.\n` +
      `  On Windows, use WSL.`
    );
  }
  return `${os}-${arch}`;
}

function resolveBinary() {
  const triple = targetTriple();
  const pkg = `@sparx/cli-${triple}`;
  const exe = process.platform === 'win32' ? 'sparx.exe' : 'sparx';

  // require.resolve finds the dep wherever npm hoisted it, which is more
  // reliable than assuming node_modules/<pkg> relative to this file
  // (pnpm and yarn PnP lay it out differently).
  let binPath;
  try {
    binPath = require.resolve(`${pkg}/bin/${exe}`);
  } catch (_) {
    // Fall back to a direct path probe before giving up: require.resolve fails
    // if the sub-package has no "exports" map for the bin path.
    const guess = path.join(__dirname, 'node_modules', pkg, 'bin', exe);
    if (fs.existsSync(guess)) {
      binPath = guess;
    } else {
      throw new Error(
        `could not find the sparx binary for ${triple}.\n` +
        `  Expected package: ${pkg}\n` +
        `  This usually means install ran with --no-optional or the optional\n` +
        `  dependency failed to download. Try:\n` +
        `    npm install --force @sparx/cli\n` +
        `  Or install without npm:\n` +
        `    curl -fsSL https://openschbrid.dev/install.sh | sh`
      );
    }
  }
  return binPath;
}

// postinstall: verify presence and ensure the exec bit. npm does not reliably
// preserve mode bits from a published tarball across all registries/proxies,
// and a binary without +x fails with a confusing EACCES at first use rather
// than at install time.
function postinstall() {
  let binPath;
  try {
    binPath = resolveBinary();
  } catch (err) {
    // Warn, don't fail the install. A hard failure here breaks `npm ci` in a
    // container that only needs the JS deps, and the launcher reports the same
    // problem with the same guidance if sparx is actually invoked.
    process.stderr.write(`\n  sparx: ${err.message}\n\n`);
    return;
  }
  if (process.platform !== 'win32') {
    try {
      fs.chmodSync(binPath, 0o755);
    } catch (err) {
      process.stderr.write(`  sparx: could not set exec bit on ${binPath}: ${err.message}\n`);
      return;
    }
  }
  process.stdout.write(`  sparx ready (${targetTriple()})\n`);
}

module.exports = { resolveBinary, targetTriple };

if (require.main === module) {
  postinstall();
}
