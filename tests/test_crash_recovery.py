"""Real process death between external fsync and receipt commit, plus store faults."""
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

probe, cli = sys.argv[1:]


def run(args, code=0, stdin=None):
    result = subprocess.run(args, input=stdin, text=True, capture_output=True, timeout=15)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return json.loads(result.stdout) if result.stdout.strip() else None


with tempfile.TemporaryDirectory(prefix='sparx crash ') as temp:
    root = Path(temp)
    for mode in ['normal', 'crash', 'throw']:
        db, effect = str(root / (mode + '.db')), root / (mode + '.effect')
        run([probe, mode, db, str(effect)], 86 if mode == 'crash' else 1 if mode == 'throw' else 0)
        assert effect.read_text() == 'effect\n'
        replay = run([probe, 'normal', db, str(effect)], 0 if mode == 'normal' else 1)
        assert effect.read_text() == 'effect\n', 'repeated external effect'
        if mode == 'normal':
            assert replay['replayed']
            continue
        assert replay['error'] == 'UNKNOWN'
        recovered = run([cli, 'recover', '--state', db])['unresolved']
        assert len(recovered) == 1 and recovered[0]['tool'] == 'counter.increment'
        resolution = 'committed' if mode == 'crash' else 'failed'
        run([cli, 'reconcile', '--state', db, '--session', 's', '--request-id', '1',
             '--outcome', resolution, '--output', '{"count":1}', '--note', 'Operator checked external ledger'])
        assert run([cli, 'recover', '--state', db])['unresolved'] == []
        final = run([probe, 'normal', db, str(effect)], 0 if resolution == 'committed' else 1)
        assert (final.get('replayed') if resolution == 'committed' else final['error'] == 'RECONCILED_FAILED')
        assert effect.read_text() == 'effect\n'
        with sqlite3.connect(db) as connection:
            audit = connection.execute("SELECT event,note FROM events WHERE event LIKE 'RECONCILED_%'").fetchall()
            assert audit == [('RECONCILED_' + ('COMMITTED' if resolution == 'committed' else 'FAILED'), 'Operator checked external ledger')]

    db, effect = str(root / 'locked.db'), str(root / 'locked.effect')
    owner = subprocess.Popen([probe, 'hold', db, effect], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        assert owner.stdout.readline().strip() == 'ready'
        assert run([probe, 'normal', db, effect], 1) == {'error': 'STORAGE_BUSY', 'run_blocked': True}
    finally:
        owner.communicate('\n', timeout=10)

    for stage in ['begin', 'dispatch', 'finish']:
        db, effect = str(root / (stage + '.db')), root / (stage + '.effect')
        run([probe, 'init', db, str(effect)])
        trigger = {'begin': 'BEFORE INSERT ON requests',
                   'dispatch': "BEFORE UPDATE ON requests WHEN NEW.state='DISPATCHED'",
                   'finish': "BEFORE UPDATE ON requests WHEN NEW.state='COMMITTED'"}[stage]
        with sqlite3.connect(db) as connection:
            connection.execute(f"CREATE TRIGGER injected_failure {trigger} BEGIN SELECT RAISE(ABORT,'injected write failure'); END")
        fault = run([probe, 'normal', db, str(effect)], 1)
        assert fault['error'] == ('STORAGE_ERROR' if stage == 'begin' else 'UNKNOWN')
        assert effect.exists() == (stage == 'finish'), 'tool ran without a durable dispatch record'
        if stage == 'finish':
            assert run([cli, 'recover', '--state', db])['unresolved'][0]['request_id'] == '1'

    db = root / 'corrupt.db'
    db.write_bytes(b'not a database')
    assert run([probe, 'normal', str(db), str(root / 'never.effect')], 1)['run_blocked']
    assert not (root / 'never.effect').exists()
    db = root / 'future.db'
    run([probe, 'init', str(db), str(root / 'unused')])
    with sqlite3.connect(db) as connection:
        connection.execute('PRAGMA user_version=999')
    assert run([probe, 'normal', str(db), str(root / 'never.effect')], 1)['error'] == 'STORE_VERSION'

    # Explicit request IDs survive separate CLI processes and JSONL sessions.
    db = str(root / 'cli.db')
    command = [cli, 'run', '--state', db, '--session', 'cli-test', '--request-id', '1', '--input', 'set AC to 22 degrees']
    assert not run(command)['replayed']
    assert run(command)['replayed']
    result = run([cli, 'run', '--state', db, '--jsonl'], stdin=json.dumps({'session_id': 'cli-test', 'request_id': '2', 'input': 'vehicle status'}) + '\n')
    assert result['ok']
    run([cli, 'run', '--state', db, '--input', 'vehicle status'], 2)

    run([cli, 'run', '--state', '', '--input', 'vehicle status'], 2)
