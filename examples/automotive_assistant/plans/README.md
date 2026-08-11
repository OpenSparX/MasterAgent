# Execution Plans

This directory contains YAML plan specs that define multi-step task orchestration
for the automotive assistant. Each file describes a directed acyclic graph (DAG)
of capabilities the agent should execute.

## Usage

```bash
# Validate a plan against the orchestrator
sparx plan validate plans/turn-off-ac.yaml

# Show the plan with visual structure
sparx plan show plans/route-briefing.yaml

# Export as Mermaid diagram (paste into GitHub markdown)
sparx plan export plans/route-briefing.yaml --format=mermaid

# Export as JSON (for programmatic consumption)
sparx plan export plans/turn-off-ac.yaml --format=json
```

## Plan Spec Format

```yaml
plan: my-plan-name
priority: p1              # p0 (safety-critical) | p1 (default) | p2 (background)
deadline_ms: 3000         # max wall-clock time for the entire plan

nodes:
  - id: step_one
    action: service.capability.name
    params:
      key: "value"

  - id: step_two
    action: another.capability
    after: [step_one]     # dependencies — step_two waits for step_one
```

## Included Plans

| File | Shape | Description |
|------|-------|-------------|
| `turn-off-ac.yaml` | Sequential (A → B) | Read temperature, then power off AC |
| `route-briefing.yaml` | Fan-in (A+B+C → D) | Navigation, ETA, and traffic in parallel, then compose briefing |

## Priority Levels

- **P0** — Safety-critical. Requires explicit `p0_authorization` with a `trusted-safety:` reference. The orchestrator rejects P0 plans without this grant.
- **P1** — Normal priority (default). No special authorization needed.
- **P2** — Background/low-priority tasks. May be deferred under load.

## Writing Your Own Plans

1. Create a `.yaml` file in this directory
2. Define nodes with `action:` pointing to registered capabilities
3. Use `after:` to express dependencies between nodes
4. Run `sparx plan validate` to check it compiles and passes the orchestrator
5. Use `sparx plan show` to visualize the structure

Plans are validated against the real orchestrator — if a rule changes in the
kernel, `sparx plan validate` catches it immediately.
