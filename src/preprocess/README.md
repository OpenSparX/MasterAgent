# Preprocessing Source Layout

The public API is not stored in this directory. Start with:

`include/master_agent/preprocess/preprocess_engine.h`

The component headers under `src/preprocess/include/` are private to the
implementation and must not be included by another module. They separate
validation, text processing, state Provider registration, state querying,
observability, and engine composition. The `.cpp` files in this directory
implement the public contracts declared by `preprocess_engine.h`.

When the module is reviewed or integrated separately, preserve both the public
`include/master_agent/preprocess/` tree and this `src/preprocess/` tree. Copying
only the implementation directory is incomplete and will not compile.

Provider ownership is one-to-one: one `StateDomain` may have only one
`IRuntimeStateProvider`. Duplicate domain registration is a configuration error
reported as `PREPROCESS_STATE_PROVIDER_DOMAIN_CONFLICT` by the state-query APIs.
