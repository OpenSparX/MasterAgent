"""Independent service commits stock; runtime recovers a lost network receipt."""
import json
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

agent, cli, service = sys.argv[1:]
# Loopback integration must not inherit a developer's HTTP proxy.
http = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def request(url, data=None, key=None):
    headers = {'Content-Type': 'application/json'}
    if key:
        headers['Idempotency-Key'] = key
    req = urllib.request.Request(url, data=None if data is None else json.dumps(data).encode(), headers=headers)
    try:
        with http.open(req, timeout=3) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def run(command, code=0):
    p = subprocess.run(command, capture_output=True, text=True, timeout=5)
    assert p.returncode == code, (command, p.stdout, p.stderr)
    return json.loads(p.stdout)


def start(root, drop=False):
    ready = root / 'ready'
    ready.unlink(missing_ok=True)
    command = [sys.executable, service, '--database', str(root / 'inventory.db'), '--port', '0', '--ready-file', str(ready)]
    if drop:
        command += ['--drop-first-response']
    process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(150):
        if ready.exists():
            return process, 'http://127.0.0.1:' + ready.read_text()
        if process.poll() is not None:
            raise AssertionError(process.stderr.read().decode())
        time.sleep(0.02)
    process.terminate()
    process.wait(timeout=5)
    raise AssertionError('Service startup timed out')


with tempfile.TemporaryDirectory(prefix='sparx-inventory-') as directory:
    root = Path(directory)
    db = str(root / 'runtime.db')
    process, endpoint = start(root, drop=True)
    try:
        args = [agent, endpoint, db, 'shop', 'order-1', 'widget', '2', '1000']
        assert run(args, 1)['error'] == 'UNKNOWN'
        assert request(endpoint + '/stock/widget')[1]['quantity'] == 8
        # Fresh process replays UNKNOWN and never sends the mutation again.
        assert run(args, 1)['error'] == 'UNKNOWN'
        pending = run([cli, 'recover', '--state', db])
        assert len(pending['unresolved']) == 1, pending
        key = 'shop'.encode().hex() + '.' + 'order-1'.encode().hex()
        receipt = request(endpoint + '/receipts/' + key)[1]
        assert receipt['outcome'] == 'committed'
        run([cli, 'reconcile', '--state', db, '--session', 'shop', '--request-id', 'order-1',
             '--outcome', 'committed', '--output', json.dumps(receipt['output']),
             '--note', 'Verified independent service receipt ' + key])
        replay = run(args)
        assert replay['replayed'] and replay['output']['remaining'] == 8
        # External idempotency is atomic with stock and detects changed arguments.
        assert request(endpoint + '/reservations', {'sku': 'widget', 'quantity': 2}, key)[0] == 200
        assert request(endpoint + '/reservations', {'sku': 'widget', 'quantity': 3}, key)[1]['outcome'] == 'conflict'
        assert request(endpoint + '/stock/widget')[1]['quantity'] == 8
        assert run([agent, endpoint, db, 'shop', 'order-2', 'widget', '20'], 1)['error'] == 'TOOL_FAILED'
        assert not run([cli, 'recover', '--state', db])['unresolved']
        assert run([agent, endpoint, db, 'shop', 'order-3', 'widget', '3'])['output']['remaining'] == 5
    finally:
        process.terminate()
        process.wait(timeout=5)
    process, endpoint = start(root)
    try:
        assert request(endpoint + '/stock/widget')[1]['quantity'] == 5
        # Even a fresh runtime store uses the stable key; the service does not
        # repeat the reservation. The service receipt also survives its restart.
        result = run([agent, endpoint, str(root / 'fresh.db'), 'shop', 'order-3', 'widget', '3'])
        assert result['output']['remaining'] == 5 and not result['replayed']
        assert request(endpoint + '/stock/widget')[1]['quantity'] == 5
    finally:
        process.terminate()
        process.wait(timeout=5)
print('Inventory integration: lost receipt, reconciliation, atomic idempotency and restart passed')
