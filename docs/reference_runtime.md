# Public reference runtime contract

The supported public entry point is `master_agent::reference::Runtime`, linked
through `MasterAgent::Core`. This alpha API does not implement legacy private
kernel factories. The HTTP adapter is a separate `MasterAgent::Http` target.

## Execution

1. Validate bounded, nonempty session ID, request ID, and input.
2. Return a cached outcome for a repeated request ID with identical input.
3. Match an exact phrase skill before considering the model.
4. Otherwise call the supplied model callback with system/tool descriptions and
   bounded conversation history; require a single JSON decision.
5. Resolve the registered tool and validate every argument before invoking it.
6. Return the actual result and record the outcome in the in-memory session.

A model can return either `{"response":"text"}` or
`{"tool":"registered.name","arguments":{...}}`. Mixed, malformed, unknown-tool,
and invalid-argument output is rejected. This release performs at most one tool
call per turn; it does not generate or execute arbitrary code, multi-step DAGs,
or a model-driven retry loop.

## Tools and schemas

Tool callbacks are trusted native application code. They are not sandboxed and
must implement domain authorization before performing a side effect. Register
only tools appropriate to the caller/session. This runtime does not supply a
multi-tenant authorization system or MCP server transport.

Schemas must use `type:object`, `properties`, and
`additionalProperties:false`. `required` is optional. Property types are
`string`, `boolean`, `integer`, and `number`; optional constraints are `enum`,
`minimum`, and `maximum`, plus descriptive strings. Unsupported keywords,
nested objects/arrays, malformed schemas, and unknown required fields are
rejected at registration. Do not assume full JSON Schema conformance.

## Context-aware tools and outcomes

`registerContextTool(ContextTool)` is the preferred integration API. The handler
receives `const ExecutionContext&` and validated arguments and returns a
`ToolOutcome`:

```cpp
runtime.registerContextTool({"reserve", "Reserve stock", schema,
    [](const ExecutionContext& context, const Json& arguments) {
        // Pass context.idempotencyKey() to a service that supports atomic deduplication.
        // Check context.stopRequested() before initiating an operation.
        return ToolOutcome::committed(Json{{"reserved", true}});
    }});
```

The snippet shows the API shape only; the [inventory example](../examples/inventory_agent/README.md)
implements the external operation and lost-response handling.

- `committed(output)`: the effect/result is confirmed. A cancellation or deadline
  arriving during the callback does not erase that confirmation.
- `failed(message)`: definitely unsuccessful; becomes `TOOL_FAILED`. Use only when
  the service establishes failure or the tool knows it performed no effect.
- `unknown(message)`: the external outcome cannot be established; becomes `UNKNOWN`
  and requires reconciliation. A timeout after sending a mutation is not proof of failure.

This alpha release preserves source compatibility for legacy callback registration;
rebuild applications against the updated SDK. Binary ABI stability is not promised.

Legacy `registerTool(Tool)` callbacks still receive arguments only. Their successful
`Result` maps to committed; explicit errors map to failed, except `UNKNOWN`, which
remains unknown. Exceptions from either handler become UNKNOWN. Prefer the typed API
for new adapters so a generic transport failure is not mistaken for a definite failure.

`run(turn, options)` accepts `RunOptions` with an optional `steady_clock` deadline
and a copyable `CancellationToken`. Copies share an atomic flag; another thread may
call `options.cancellation.cancel()` while the synchronous run is in progress.
A tool should poll `context.stopRequested()` or integrate the signal with its I/O.
Native code is not forcibly interrupted, and cancellation cannot undo a remote effect.
Custom model handlers still own their I/O timeout; the runtime checks again before
invoking any selected tool.

For an unseen request, cancellation/deadline is checked before recording STARTED,
then before dispatch and invocation. A pre-start rejection does not consume a session
or request slot; a stop after STARTED is recorded as a terminal failure. Replaying an
existing receipt returns that authoritative result even if the new options are expired
or cancelled. Deliberately retrying a recorded failure needs a new request ID.

`idempotencyKey()` encodes UTF-8 session/request bytes separately in hex with a dot
separator. It is stable across restarts and collision-free for the bounded IDs accepted
by `run()`. Its namespace is one host application/store; independent applications must
provide distinct external scopes. Session/request IDs are not credentials. Authorization
and tenant isolation remain the host's responsibility. Do not retain references to the
context/arguments after the synchronous callback returns; copy needed values explicitly.

## Sessions, replay, and failure

Defaults: 32 sessions, 256 recorded requests per session, 8 successful turns of
model history, 64 KiB input/model text, and 256-byte IDs. A full session returns
`RESOURCE_LIMIT`; it never silently evicts request IDs and repeats a tool.
`clearSession()` explicitly removes both history and replay protection.

Request IDs are scoped to the session. Reusing an ID with different input
returns `REQUEST_CONFLICT`. Both success and failure are cached. For a deliberate
retry, the caller must inspect the outcome and provide a new ID. With no store, state is memory-only and deduplication does not survive restart.
Call `openStore(path)` before creating sessions to persist receipts and history in
SQLite. Recovery is conservative: interrupted requests become UNKNOWN and cannot
be automatically retried. See [durable recovery](durable_recovery.md).
A handler exception returns `UNKNOWN`, because its side effect may already have
occurred. Repeating that ID returns the same error without invoking the handler
again. Tool handlers should return explicit `Result` failures when the outcome
is known. Use `unresolved()` to inspect ambiguous operations and `reconcile()` to record an
operator-verified outcome and evidence note. `clearSession()` refuses unknown
outcomes. The store does not encrypt data and does not implement a multi-step DAG.

## Concurrency and timeouts

Runtime methods use a nonblocking instance lock. Concurrent or reentrant calls
return `BUSY`; they do not race or deadlock on a recursive tool call. Tools and
model callbacks run synchronously. The host must bound their runtime; native
callbacks cannot be safely preempted by this library.

The optional libcurl model callback has a finite HTTP timeout (30 seconds by
default), disables redirects, verifies TLS using libcurl defaults, and limits
responses to 1 MiB. Credentials are supplied through the SDK config or a CLI-named environment
variable, never a CLI credential argument. Endpoints are explicit; the reference CLI defaults to no model.

The separate experimental edge/cloud harness uses a common inference deadline
and retains shared backend ownership for late workers. It permits at most one
outstanding local and one outstanding cloud inference per instance; a timed-out
worker must finish before that lane admits another task. It does not forcibly
cancel third-party synchronous callbacks. Prompt compression, scoring and
arbitration callbacks must be fast and thread-safe. Direct `executeLocalOnly`
and `executeCloudOnly` methods remain synchronous diagnostic calls.

## Verification boundaries

CI tests the real HTTP client against a loopback HTTP protocol fixture, including
Unicode, malformed payloads, unknown tools, invalid arguments and HTTP failures.
That proves transport and dispatch behavior, not model quality. A real GGUF model
and supported hardware are required for model/NPU performance claims.

The legacy learning store uses XOR obfuscation and the bundled legacy journal
contains incomplete persistence methods. Neither is used by this reference
runtime. They must not be used as production encryption or crash recovery.
