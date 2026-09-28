"""Verify success, failed evaluations and missing binaries with isolated fixtures."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='sparx eval ') as temp:
    root = Path(temp)
    (root / 'eval').mkdir()
    shutil.copyfile(sys.argv[1], root / 'eval/run_all.sh')
    (root / 'bin').mkdir()
    cmake = root / 'bin/cmake'
    cmake.write_text('#!/bin/sh\nexit 0\n')
    cmake.chmod(0o755)
    binaries = root / 'build/eval'
    binaries.mkdir(parents=True)
    names = ['speculation', 'mesh', 'formal', 'learning', 'constrained']
    for name in names:
        p = binaries / ('eval_' + name)
        p.write_text('#!/bin/sh\necho fixture\nexit 0\n')
        p.chmod(0o755)
    env = dict(os.environ, PATH=str(root / 'bin') + os.pathsep + os.environ['PATH'])
    env.pop('SPARX_BUILD_DIR', None)
    env.pop('SPARX_RESULTS_DIR', None)
    def run():
        return subprocess.run(['bash', str(root / 'eval/run_all.sh')], env=env, capture_output=True, text=True)
    good = run()
    assert good.returncode == 0 and '5 passed, 0 failed' in good.stdout, good
    (binaries / 'eval_mesh').write_text('#!/bin/sh\nexit 7\n')
    bad = run()
    assert bad.returncode != 0 and '4 passed, 1 failed' in bad.stdout, bad
    (binaries / 'eval_formal').unlink()
    missing = run()
    assert missing.returncode != 0 and '3 passed, 2 failed' in missing.stdout, missing
    summary = (root / 'build/eval-results/SUMMARY.md').read_text()
    assert '| eval_mesh | FAIL |' in summary and '| eval_formal | SKIP |' in summary
