# Inventory reservation over HTTP

This is a runnable local integration example, not a mock callback: the agent
contacts an independent Python service which atomically updates stock and a
receipt in its own SQLite database. It starts with 10 units of `widget`.
No model, external account or credentials are required. Python 3.9+, SQLite and
libcurl are required. The service is unauthenticated and binds only to loopback;
do not expose it publicly. It is not a production inventory server.

After installing the SDK (see the repository README):

```bash
cmake -S examples/inventory_agent -B build-inventory -DCMAKE_PREFIX_PATH="$PWD/install-sdk"
cmake --build build-inventory
python3 examples/inventory_agent/service.py --database inventory.db --port 8765
```

In another terminal:

```bash
./build-inventory/inventory_agent http://127.0.0.1:8765 agent.db shop order-1 widget 2
# Repeat: replayed:true; inventory is not decremented twice.
curl http://127.0.0.1:8765/stock/widget
```

The CLI takes `BASE_URL STATE_DB SESSION REQUEST SKU QUANTITY [TIMEOUT_MS]`.
A request ID identifies one immutable reservation in a session. Do not reuse it
for a different item/quantity. Both sides must agree on the application/store
namespace: the sample key encodes the session and request bytes as hex separated
by a dot. Independent tenants must use distinct scopes/stores. These IDs are not
authentication or authorization credentials.

## Reproduce a lost response

Start a fresh service database with `--drop-first-response`. Submit a new request.
The service commits the stock change and closes the socket before replying. The
agent returns `UNKNOWN`; submitting the same IDs again, including after restart,
returns the same uncertainty without sending another reservation.

```bash
sparx recover --state agent.db
# For session=shop and request=order-1:
curl http://127.0.0.1:8765/receipts/73686f70.6f726465722d31
```

Inspect the authoritative receipt and compare its key, SKU and quantity with the
unresolved request. Only then reconcile using the receipt's `output`:

```bash
sparx reconcile --state agent.db --session shop --request-id order-1 \
  --outcome committed --output '{"sku":"widget","quantity":2,"remaining":8}' \
  --note 'Verified the matching committed inventory service receipt'
```

Use the actual receipt, not the example output, when reconciling. A missing receipt
alone does not prove failure while a remote operation could still be in flight.
The automated `inventory_service_contract` verifies this sequence, rejected stock
requests, conflicting idempotency keys and independent service/runtime restarts.

## What the adapter demonstrates

- `registerContextTool()` receives stable request identity, deadline and a shared
  cancellation token. The idempotency key is sent to the service unchanged.
- The service commits its deduplication record and stock update in one transaction.
- `ToolOutcome::committed` requires a matching service receipt. A confirmed stock
  rejection is `failed`; transfer errors, malformed receipts and key conflicts are
  `unknown`. There is no automatic retry of uncertain effects.
- libcurl observes a bounded timeout and cooperative cancellation. Cancellation
  after sending does not prove the remote service stopped or rolled back.
- Neither this example nor the runtime creates a transaction spanning the two
  databases. Real services need their own authentication, idempotency guarantees,
  retention policy and receipt-query interface.
