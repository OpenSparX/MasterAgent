#!/usr/bin/env python3
"""Write non-secret provenance for a packaged build; never dump the full CMake cache."""
import json
from pathlib import Path
import platform
import subprocess
import sys

root, build, output = map(Path, sys.argv[1:])
def git(*args):
    result = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None

cache = {}
for line in (build / 'CMakeCache.txt').read_text().splitlines():
    if line and not line.startswith(('#', '//')) and ':' in line and '=' in line:
        key, value = line.split('=', 1)
        cache[key.split(':', 1)[0]] = value

manifest = {
    'schema_version': 1,
    'version': json.loads((root / 'VERSION.json').read_text())['version'],
    'source_commit': git('rev-parse', 'HEAD'),
    'working_tree_dirty': bool(git('status', '--porcelain')),
    'platform': platform.system(),
    'architecture': platform.machine(),
    'build_type': cache.get('CMAKE_BUILD_TYPE'),
    'features': {key: cache.get(key) == 'ON' for key in ('MASTER_AGENT_ENABLE_HTTP', 'MASTER_AGENT_ENABLE_STORAGE')},
}
output.write_text(json.dumps(manifest, indent=2) + '\n')
