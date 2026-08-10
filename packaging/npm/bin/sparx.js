#!/usr/bin/env node
//
// Thin launcher. Resolves the platform-specific binary and hands the process
// over to it.
//
// The important detail here is that this is NOT a wrapper that stays alive
// relaying stdio. `sparx shell` is an interactive REPL and `sparx run` streams
// output; a Node middleman would break terminal detection (isatty), swallow
// signals, and mangle exit codes. So:
//   - on POSIX we execve() over the top of the Node process via child_process
//     with stdio:'inherit', then mirror the child's exit status exactly,
//     including death-by-signal.
//   - the binary sees a real TTY, real signals, and owns the terminal.
//
'use strict';

const { spawnSync } = require('child_process');
const { resolveBinary } = require('../install.js');

let binary;
try {
  binary = resolveBinary();
} catch (err) {
  process.stderr.write(`sparx: ${err.message}\n`);
  process.exit(1);
}

const result = spawnSync(binary, process.argv.slice(2), {
  stdio: 'inherit',
  // Windows needs shell:false explicitly or .exe resolution gets odd; POSIX
  // does not care. Set for both so behaviour is identical.
  shell: false,
});

if (result.error) {
  process.stderr.write(`sparx: failed to launch: ${result.error.message}\n`);
  process.exit(1);
}

// Propagate death-by-signal as the conventional 128+signo rather than
// reporting success. Ctrl-C in `sparx shell` must not look like a clean exit
// to a calling script.
if (result.signal) {
  const signals = { SIGINT: 2, SIGTERM: 15, SIGKILL: 9, SIGHUP: 1, SIGQUIT: 3 };
  process.exit(128 + (signals[result.signal] || 0));
}

process.exit(result.status === null ? 1 : result.status);
