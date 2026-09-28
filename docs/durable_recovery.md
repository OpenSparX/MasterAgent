# Durable requests and crash recovery

The public reference runtime can persist request receipts and successful conversation
history in SQLite. Enable `MASTER_AGENT_ENABLE_STORAGE` (default ON for the public
build), then call `Runtime::openStore(path)` before creating sessions. Linux/macOS
are supported. A failed open blocks subsequent execution; there is no silent fallback
to memory. An instance owns the store until its destructor runs.

## State transitions

`STARTED` is committed before routing/model execution. After a tool name and arguments
pass validation, `DISPATCHED` is committed **before invoking the callback**. Completion
atomically commits the receipt, bounded session history and an audit event as
`COMMITTED`, `FAILED` or `UNKNOWN`.

SQLite uses WAL and `synchronous=FULL`. File permissions are restricted to the owner.
A separate ownership lock prevents a second cooperating runtime/process from opening
the same store while work is active. The lock file must remain present. Database
symlinks/hard links are rejected; use a local filesystem, not NFS or shared storage.

When reopening an exclusively owned store, unfinished `STARTED`/`DISPATCHED` requests
become `UNKNOWN`. This is deliberately conservative: the external system may have
performed a side effect before the process stopped. Repeating the same session/request
ID returns the stored outcome without rerunning the model or tool. A different input
with that ID returns `REQUEST_CONFLICT`.

This is **not a distributed exactly-once guarantee**. There is no atomic transaction
spanning a device API and SQLite. Where a tool supports an idempotency key, the host
should pass the request identity through to that system as well. This release handles
one tool per turn; it is not a durable multi-node DAG scheduler.

## CLI

```bash
sparx run --state ./state/agent.db --session alice --request-id command-001 \
  --input 'set AC to 22 degrees'
# The same invocation after a restart returns replayed:true.
sparx recover --state ./state/agent.db
```

The AC example remains an in-memory **simulator**. A durable receipt records what a
previous invocation returned; it does not restore physical/device or simulator state.
Use the installed C++ example and crash tests to study the runtime/tool boundary.

Durable CLI requests require explicit IDs. For a long-running process:

```bash
printf '%s\n' '{"session_id":"alice","request_id":"command-002","input":"vehicle status"}' \
  | sparx run --state ./state/agent.db --jsonl
```

`recover` reports the input, selected tool and validated arguments for unresolved
requests. Inspect the external system before choosing an outcome:

```bash
sparx reconcile --state ./state/agent.db --session alice --request-id command-001 \
  --outcome committed --output '{"temperature":22}' \
  --note 'Verified the device reports the requested state'
# Or: --outcome failed --note 'Device audit confirms no operation occurred'
```

Reconciliation never invokes a tool. It records the operator's conclusion and evidence
note; the runtime cannot establish that evidence itself. Only `UNKNOWN` receipts can
be reconciled. Notes must be nonempty. Final outcomes cannot be rewritten through this
API. `clearSession()` refuses unresolved requests and explicitly discards the replay
protection of resolved ones; audit events remain in the database.

## Failures and storage maintenance

- A failed write before dispatch prevents the callback from running.
- Failure to commit a completion returns `UNKNOWN` and blocks further execution on
  that instance. Destroy it, fix the storage problem, reopen, and reconcile.
- Corrupt, unrelated, or future-version databases fail closed. Back up and inspect
  them; do not delete the database to make an ambiguous request run again.
- A second owner gets `STORAGE_BUSY`; a failed open never executes in memory.
- Session/request limits are preserved across restarts; receipts are never silently
  evicted. Operators own retention and storage capacity planning.

The database includes prompt text, arguments, outputs and notes. It is **not encrypted**.
Use operating-system storage protection as required by the deployment. Do not edit,
replace, copy, or delete a live database or its `.lock`/`-wal`/`-shm` files. For a simple
backup, stop the runtime and copy the closed database. Restoring an older backup also
restores older replay knowledge; reconcile external operations since that backup before
resuming side-effecting tools. A SQLite-aware online backup requires application-level
coordination beyond this reference release.

## Tests

`test_durable_runtime` covers reopen/replay, history restoration, explicit reconciliation,
limits and exclusive ownership. `crash_recovery_contract` starts a subprocess, fsyncs an
external effect and exits before the receipt commits. A fresh process must report
`UNKNOWN` and must not produce a second effect. It also injects database failures before
start, before dispatch and on completion, and checks corruption/version rejection.
These are process-crash tests, not destructive hardware power-loss tests.
