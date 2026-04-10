# FLUX OS — CLI Reference

## Overview

The `flux` command-line tool is the primary interface for developing, building, deploying, and managing FLUX OS applications and device fleets. It's designed for composability — every command produces structured output that can be piped to other tools, parsed by scripts, or consumed by CI/CD systems.

---

## Global Options

```
flux [global-options] <command> [subcommand] [options] [arguments]

Global Options:
  --config FILE        Path to configuration file (default: ~/.flux/config.toml)
  --format FORMAT      Output format: table, json, tsv, quiet (default: table)
  --verbose, -v        Increase verbosity (can be repeated: -vvv)
  --quiet, -q          Suppress all output except errors
  --no-color           Disable colored output
  --help, -h           Show help for a command
  --version            Show FLUX OS CLI version
```

---

## Build Commands

### `flux build`

Compile a FLUX.MD source file or project into bytecode or native code.

```bash
flux build [options] [SOURCE]

Options:
  --source, -s FILE    Source FLUX.MD file (default: ./FLUX.MD)
  --target TARGET      Target architecture: bytecode, c, native, arm64, x86_64, riscv64, wasm32
  --output, -o FILE    Output file path (default: ./build/<name>.fluxbc)
  --optimize, -O LEVEL Optimization level: 0, 1, 2, 3, s (size), z (min size)
  --board BOARD        Target board for board-specific optimizations
  --debug-symbols      Include debug symbols in output
  --no-strip          Don't strip symbol table
  --dry-run           Show what would be built without actually building

Examples:
  flux build                              # Build ./FLUX.MD for current arch
  flux build -s app.flux.md               # Build specific source file
  flux build --target arm64 --board rpi4  # Cross-compile for Raspberry Pi
  flux build --target bytecode -Os        # Build portable bytecode, size-optimized
  flux build --format json                # JSON output for scripting
```

### `flux compile`

Invoke the self-compiler directly (lower-level than `flux build`).

```bash
flux compile [options]

Options:
  --input FILE        Input file (FLUX.MD or FIR text format)
  --input-lang LANG   Input language: flux_md, c, assembly, bytecode, fir
  --output-format FMT Output format: bytecode, c, native, ir, obj
  --module-name NAME  Module name for the compilation unit
  --opt-level N       Optimization level (0-3)
  --emit-fir          Emit FIR IR as text (for debugging)
  --dump-ast          Dump AST after parsing (for debugging)

Examples:
  flux compile --input app.flux.md --output-format bytecode --opt-level 2
  flux compile --input app.flux.md --emit-fir --dump-ast  # Debug pipeline
```

---

## Deploy Commands

### `flux deploy`

Deploy a compiled artifact to devices or fleets.

```bash
flux deploy [options] ARTIFACT

Options:
  --device NAME       Deploy to a specific device
  --fleet NAME        Deploy to a fleet
  --strategy STRAT    Deployment strategy: rolling, canary, blue-green, all-at-once
  --batch-size N      Percentage or count for rolling deployment (default: 25%)
  --canary-count N    Number of canary devices (default: 1)
  --canary-wait DUR   How long to wait between canary batches (default: 10m)
  --health-check      Enable automatic health check after deployment
  --health-interval   Health check interval (default: 30s)
  --health-timeout    Health check timeout before rollback (default: 5m)
  --rollback-on-fail  Automatic rollback on health check failure (default: true)
  --dry-run           Show what would be deployed without deploying
  --confirm           Require confirmation before deploying

Examples:
  flux deploy build/app.fluxbc --device greenhouse-pi-01
  flux deploy build/app.fluxbc --fleet greenhouse --strategy rolling --batch-size 10%
  flux deploy build/app.fluxbc --fleet production --strategy canary --confirm
```

### `flux deploy status`

Check the status of a deployment.

```bash
flux deploy status [options]

Options:
  --id ID             Deployment ID (if not specified, shows latest)
  --follow, -f        Follow deployment progress in real-time
  --show-failures     Show detailed failure information
  --format FORMAT     Output format

Examples:
  flux deploy status --follow
  flux deploy status --id deploy-12345 --show-failures --format json
```

### `flux deploy rollback`

Rollback a deployment to a previous version.

```bash
flux deploy rollback [options]

Options:
  --fleet NAME        Fleet to rollback
  --device NAME       Specific device to rollback
  --to-version VER    Version to rollback to (default: previous version)
  --strategy STRAT    Rollback strategy (default: rolling)

Examples:
  flux deploy rollback --fleet greenhouse
  flux deploy rollback --device greenhouse-pi-03 --to-version 1.5.0
```

---

## Device Commands

### `flux device list`

List all registered devices.

```bash
flux device list [options]

Options:
  --fleet NAME        Filter by fleet
  --status STATUS     Filter by status: online, offline, error, all
  --format FORMAT     Output format

Examples:
  flux device list
  flux device list --fleet greenhouse --status online --format json
```

### `flux device status`

Get detailed status of a device.

```bash
flux device status --device NAME [options]

Options:
  --verbose, -v       Show detailed hardware and software information
  --format FORMAT     Output format

Examples:
  flux device status --device greenhouse-pi-01
  flux device status --device greenhouse-pi-01 --verbose --format json
```

### `flux device health`

Run health checks on a device.

```bash
flux device health --device NAME [options]

Options:
  --check CHECK       Specific check: heartbeat, process, memory, network, all
  --timeout SECS      Timeout for health check (default: 10)
  --quiet, -q         Exit code only (0=healthy, 1=degraded, 2=unhealthy, 3=unreachable)

Examples:
  flux device health --device greenhouse-pi-01
  flux device health --device greenhouse-pi-01 --quiet && echo "Healthy"
```

### `flux device logs`

Stream logs from a device.

```bash
flux device logs --device NAME [options]

Options:
  --follow, -f        Follow log output (like tail -f)
  --since DURATION    Show logs since duration (e.g., 1h, 30m, 24h)
  --level LEVEL       Filter by level: debug, info, warn, error
  --grep PATTERN      Filter by pattern
  --output FILE       Export logs to file (JSONL format)
  --tail N            Show last N lines (default: 100)

Examples:
  flux device logs --device greenhouse-pi-01 --follow
  flux device logs --device greenhouse-pi-01 --since 1h --level error
  flux device logs --device greenhouse-pi-01 --grep "temperature" --output temp-logs.jsonl
```

### `flux device shell`

Open a remote shell on a device.

```bash
flux device shell --device NAME [options]

Options:
  --command CMD       Run a single command instead of interactive shell
  --timeout SECS      Command timeout (default: 30)

Examples:
  flux device shell --device greenhouse-pi-01
  flux device shell --device greenhouse-pi-01 --command "flux_kernel_info"
```

---

## Fleet Commands

### `flux fleet list`

List all fleets.

```bash
flux fleet list [options]

Options:
  --format FORMAT     Output format

Examples:
  flux fleet list
  flux fleet list --format json
```

### `flux fleet status`

Get status of a fleet.

```bash
flux fleet status --fleet NAME [options]

Options:
  --verbose           Show per-device status
  --format FORMAT     Output format

Examples:
  flux fleet status --fleet greenhouse
  flux fleet status --fleet greenhouse --verbose --format json
```

### `flux fleet metrics`

Get fleet metrics.

```bash
flux fleet metrics --fleet NAME [options]

Options:
  --metric NAME       Specific metric (default: all)
  --since DURATION    Time range (e.g., 1h, 24h, 7d)
  --interval SECS     Aggregation interval (default: 60)
  --aggregate AGG     Aggregation: avg, min, max, sum, p50, p99
  --follow, -f        Stream metrics in real-time
  --format FORMAT     Output format

Examples:
  flux fleet metrics --fleet greenhouse --follow
  flux fleet metrics --fleet greenhouse --metric latency_p99 --since 24h --aggregate p99
```

### `flux fleet exec`

Execute a command on all devices in a fleet.

```bash
flux fleet exec --fleet NAME --command CMD [options]

Options:
  --strategy STRAT    Execution strategy: parallel, rolling, batch
  --batch-size N      Batch size for rolling/batch strategy
  --timeout SECS      Per-device command timeout
  --format FORMAT     Output format

Examples:
  flux fleet exec --fleet greenhouse --command "flux_log_level debug"
  flux fleet exec --fleet greenhouse --command "flux_agent_restart temp-monitor" --strategy rolling
```

---

## A/B Test Commands

### `flux ab-test run`

Create and start an A/B test.

```bash
flux ab-test run [options]

Options:
  --name NAME         Test name (auto-generated if omitted)
  --variant-a FILE    Control variant bytecode
  --variant-b FILE    Treatment variant bytecode
  --label-a TEXT      Label for variant A (default: "control")
  --label-b TEXT      Label for variant B (default: "treatment")
  --split RATIO       Device split ratio (default: 50/50)
  --fleet NAME        Target fleet
  --duration DUR      Test duration (default: 24h)
  --metrics LIST      Comma-separated metrics to collect
  --significance NUM  Statistical significance threshold (default: 0.95)
  --min-samples N     Minimum samples before analysis (default: 100)
  --auto-promote      Auto-promote winner when significant
  --auto-rollback     Auto-rollback if treatment regresses (default: true)

Examples:
  flux ab-test run --variant-a v1.fluxbc --variant-b v2.fluxbc --fleet production
  flux ab-test run --name latency-test --variant-a v1.fluxbc --variant-b v2.fluxbc \
    --split 80/20 --duration 48h --metrics latency_p99,error_rate,memory_bytes
```

### `flux ab-test watch`

Watch A/B test progress in real-time.

```bash
flux ab-test watch --name NAME [options]

Options:
  --interval SECS     Refresh interval (default: 5)
  --no-clear          Don't clear screen between refreshes

Examples:
  flux ab-test watch --name my-test
```

### `flux ab-test promote`

Promote the winning variant.

```bash
flux ab-test promote --name NAME [options]

Options:
  --winner a|b        Which variant to promote (auto-detected if omitted)
  --strategy STRAT    Rollout strategy (default: rolling)

Examples:
  flux ab-test promote --name my-test --winner b
  flux ab-test promote --name my-test  # Auto-detect winner
```

### `flux ab-test rollback`

Rollback an A/B test.

```bash
flux ab-test rollback --name NAME [options]

Options:
  --to-version VER    Rollback to specific version

Examples:
  flux ab-test rollback --name my-test
```

### `flux ab-test results`

Get A/B test results.

```bash
flux ab-test results --name NAME [options]

Options:
  --format FORMAT     Output format: table, json
  --export FILE       Export raw data to CSV/JSON file

Examples:
  flux ab-test results --name my-test
  flux ab-test results --name my-test --format json --export results.json
```

---

## Hot-Swap Commands

### `flux hot-swap`

Hot-swap bytecode on a running device.

```bash
flux hot-swap [options] BYTECODE_FILE

Options:
  --device NAME       Target device (required)
  --agent-id ID       Specific agent to swap (default: all agents on device)
  --timeout MS        Swap timeout in milliseconds (default: 5000)
  --verify-only       Only verify new bytecode, don't swap
  --rollback-on-fail  Automatic rollback on swap failure (default: true)
  --format FORMAT     Output format

Examples:
  flux hot-swap build/v2.fluxbc --device greenhouse-pi-01
  flux hot-swap build/v2.fluxbc --device greenhouse-pi-01 --agent-id 42 --format json
```

---

## Agent Commands

### `flux agent list`

List agents on a device.

```bash
flux agent list [options]

Options:
  --device NAME       Target device (default: localhost)
  --state STATE       Filter by agent state
  --format FORMAT     Output format

Examples:
  flux agent list
  flux agent list --device greenhouse-pi-01 --format json
```

### `flux agent info`

Get detailed agent information.

```bash
flux agent info --agent-id ID [options]

Options:
  --device NAME       Target device
  --format FORMAT     Output format

Examples:
  flux agent info --agent-id 42
  flux agent info --agent-id 42 --device greenhouse-pi-01 --format json
```

---

## Configuration

### `flux config`

Manage CLI configuration.

```bash
flux config [options]

Subcommands:
  flux config get KEY           Get a config value
  flux config set KEY VALUE     Set a config value
  flux config list              List all config values
  flux config reset             Reset to defaults

Examples:
  flux config get deploy.default_strategy
  flux config set format json
  flux config list
```

### Configuration File

The CLI reads configuration from `~/.flux/config.toml` (or a file specified with `--config`):

```toml
[build]
default_target = "bytecode"
default_optimize = "O2"
output_dir = "./build"

[deploy]
default_strategy = "rolling"
default_batch_size = "25%"
health_check = true
health_timeout = "5m"
rollback_on_fail = true

[device]
default_timeout = "10s"
log_level = "info"

[format]
default = "table"
no_color = false
```
