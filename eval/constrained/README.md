# Constrained Decoding Evaluation

## Overview

This evaluation tests the `GbnfGenerator` component of OpenSparX's constrained
decoding module. The generator converts MCP tool schemas (JSON Schema subset)
into GBNF grammars that are fed to llama.cpp's sampler to guarantee valid
tool-call output from the LLM.

## Methodology

### Test Schemas (15+ realistic MCP tools)

| # | Tool | Complexity | Key Features |
|---|------|-----------|--------------|
| 1 | set_alarm | Simple (2 fields) | enum, required |
| 2 | send_message | Simple (3 fields) | optional boolean |
| 3 | weather | Medium (5 fields) | enum, integer, optional |
| 4 | file_operations | Medium (5 fields) | 5-value enum |
| 5 | database_query | Medium (6 fields) | array of strings |
| 6 | calendar_event | Complex (11 fields) | nested objects, array of objects, recurrence |
| 7 | http_request | Complex (8 fields) | nested array of objects, 7-value enum |
| 8 | smart_home | Union (8 fields) | 4 device types, multi-action enum |
| 9 | code_execution | Medium (6 fields) | array of objects, 6-value enum |
| 10 | deploy_service | Complex (12 fields) | nested config object, arrays |
| 11 | search | Simple (4 fields) | single required field |
| 12 | email_compose | Medium (7 fields) | multiple arrays |
| 13 | data_pipeline | Complex (9 fields) | nested source/destination, transform arrays |
| 14 | notification | Union (7 fields) | 4 channel types, nested metadata |
| 15 | k8s_resource | Complex (11 fields, deep) | 4-level nesting, arrays of objects |
| 16 | feature_flag | Simple (3 fields) | number type |

### Stress Test Schemas

| Schema | Purpose |
|--------|---------|
| 50+ fields | Tests scaling with many properties, mixed nested/flat |
| File tree (recursive) | 3-level manual unrolling of recursive structure |
| 120-value enum | Tests grammar size with many alternatives |
| Empty schema | Edge case: no properties defined |
| All-optional fields | Edge case: no required fields |

## Metrics Measured

### 1. Grammar Correctness (%)
Percentage of schemas that produce syntactically valid GBNF. Validates:
- At least one rule defined
- A `root` rule exists
- No empty rule bodies
- Rule definitions follow `name ::= body` format

**Target:** >= 90%

### 2. Schema Coverage (%)
Percentage of JSON Schema features correctly mapped to GBNF:
- `required` fields (mandatory in grammar)
- `enum` values (literal string alternatives)
- `array` type (with typed items)
- Nested `object` types
- Array of objects (typed array items)
- Primitive types: `boolean`, `integer`, `number`, `string`
- Optional fields (conditional presence)
- Multi-tool union (root rule with `|` alternatives)
- Free-text enable/disable

**Target:** >= 80%

### 3. Grammar Size vs Schema Complexity
Measures how grammar byte size scales with number of schema fields.
Validates sub-exponential growth (ratio test: grammar size growth should be
less than 10x the field count growth).

### 4. Decode Validity Rate (%)
Structural verification that grammars guarantee valid JSON output:
- Tool-call envelope (`"tool"`, `"arguments"` keys) present in all tool rules
- All property rules resolve to typed terminals
- Root rule constrains output to defined alternatives only

**Target:** >= 90%

### 5. Generation Overhead (ms)
Timing of grammar generation across complexity levels. Measured over 100
iterations after warmup. Reports mean, p99, and max.

| Complexity | Target |
|-----------|--------|
| Simple (2 fields) | < 1 ms |
| Medium (5 fields) | < 2 ms |
| Complex (11 fields) | < 5 ms |
| Very Complex (deep) | < 10 ms |
| Stress (50 fields) | < 10 ms |
| Large Enum (120) | < 5 ms |

### 6. Constrained vs Unconstrained Comparison
Simulates common LLM malformation patterns (12 examples) that constrained
decoding prevents by construction:
- Missing closing braces
- Trailing text after JSON
- Hallucinated key names
- Unquoted strings
- Missing required fields
- Invalid enum values
- Wrong types
- Markdown wrappers
- Explanation prefixes

Reports the percentage-point improvement over unconstrained generation.

## Building and Running

```bash
cd /tmp/sparx-work/eval/constrained

g++ -std=c++17 -O2 \
    -I../../cli/include \
    -o eval_constrained \
    eval_constrained.cpp \
    ../../cli/src/sparx_constrained_decode.cpp

./eval_constrained          # Normal output
./eval_constrained --verbose  # Show individual PASS results
```

### Dependencies

- C++17 compiler (GCC 8+, Clang 7+, MSVC 19.14+)
- nlohmann/json (header-only, already in project includes)
- No external test framework required

## Interpreting Results

The evaluation produces a summary table of all metrics with PASS/FAIL status.
Exit code 0 means all tests passed; exit code 1 means at least one failure.

Key signals:
- **Grammar Correctness < 90%**: Schema-to-GBNF conversion has bugs for certain
  schema shapes. Check which schemas fail and inspect the generated grammar.
- **Overhead > 10ms**: Grammar generation may have algorithmic inefficiency
  (likely in deeply nested schemas). Profile `objectToGbnf` recursion.
- **Schema Coverage gaps**: Unsupported JSON Schema features need implementation
  (e.g., `oneOf`, `allOf`, `$ref`, `patternProperties`).

## Limitations

- Cannot test actual LLM sampling (would require llama.cpp integration test)
- Recursive schemas are approximated with manual unrolling (3 levels)
- `oneOf`/`allOf`/`$ref` are not yet supported by the generator
- Timing results depend on hardware; absolute thresholds may need adjustment
