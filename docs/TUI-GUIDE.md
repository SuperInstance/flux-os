# FLUX OS — TUI Guide

## Overview

The FLUX OS Terminal User Interface (TUI) provides an interactive, keyboard-driven development experience for building, deploying, and monitoring FLUX OS applications and device fleets. It runs entirely in your terminal — no GUI, no browser, no X11 — and is designed for developers who want a rich visual interface without leaving their workflow.

The TUI is built around a tabbed layout with split panes, real-time updates, and keyboard shortcuts inspired by tools like htop, tmux, and k9s. Every action available in the CLI is also available in the TUI, but presented visually with live status indicators, progress bars, and formatted tables.

---

## Launching the TUI

```bash
# Launch the TUI (connects to localhost fleet manager)
flux tui

# Connect to a remote fleet manager
flux tui --server 192.168.1.100:9090

# Start with a specific tab
flux tui --tab deploy

# Dark/light theme
flux tui --theme dark    # default
flux tui --theme light
```

---

## Keyboard Shortcuts

### Global Shortcuts

These shortcuts work from any tab:

| Key | Action |
|-----|--------|
| `?` | Show help overlay (all shortcuts) |
| `1-9` | Switch to tab 1-9 |
| `0` | Switch to last tab |
| `Tab` / `Shift+Tab` | Next / Previous tab |
| `q` | Quit (prompts if unsaved changes) |
| `Ctrl+C` | Force quit |
| `Ctrl+L` | Redraw screen |
| `Ctrl+S` | Save current view to log file |
| `/` | Search (device names, agent names, log messages) |
| `n` / `N` | Next / Previous search result |
| `d` | Toggle detail panel (show/hide right pane) |
| `Esc` | Close overlay / Cancel / Back |

### Navigation

| Key | Action |
|-----|--------|
| `↑` / `↓` | Move selection up / down |
| `PgUp` / `PgDn` | Scroll up / down one page |
| `Home` / `End` | Jump to top / bottom |
| `g` | Go to top |
| `G` | Go to bottom |
| `←` / `→` | Switch between panes (left/right) |

### Common Actions

| Key | Action |
|-----|--------|
| `Enter` | Select / Expand / Open details |
| `Space` | Toggle selection (for multi-select) |
| `a` | Action menu (context-dependent actions) |
| `e` | Edit (open editor for config/source) |
| `r` | Refresh current view |
| `R` | Refresh all views |
| `f` | Toggle follow mode (auto-scroll new items) |
| `F` | Toggle fullscreen (expand current pane) |

---

## Tabs

### Tab 1: Dashboard

The dashboard provides an at-a-glance overview of your FLUX OS environment.

```
┌─ FLUX OS TUI ── Dashboard ──────────────────────────────────────────┐
│                                                                      │
│ ┌─ System ──────────────────────┐  ┌─ Fleets ───────────────────┐  │
│ │ Version:  0.1.0              │  │ greenhouse    ● 12 devices  │  │
│ │ Arch:     arm64 (native)     │  │ factory       ◐  8 devices  │  │
│ │ Uptime:   14d 6h 32m        │  │ dev           ○  2 devices  │  │
│ │ HAL:      READY              │  │                           │  │
│ │ VM:       READY              │  │ Total: 22 devices online   │  │
│ │ Compiler: READY              │  │ Total:  3 devices offline  │  │
│ │ Agents:   42 active          │  │                           │  │
│ └──────────────────────────────┘  └────────────────────────────┘  │
│                                                                      │
│ ┌─ Recent Activity ──────────────────────────────────────────────┐  │
│ │ 14:23:01  [deploy] greenhouse-pi-01 → v2.1.0 OK (3.2s)      │  │
│ │ 14:22:58  [ab-test] sensor-latency-001: B is 36.6% faster    │  │
│ │ 14:22:45  [health]  greenhouse-pi-03: memory warning (92%)   │  │
│ │ 14:22:30  [agent]   temp-monitor-01: heartbeat OK             │  │
│ │ 14:22:15  [compile] app.flux.md → arm64 OK (1.2s, 48KB)      │  │
│ └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│ ┌─ Active A/B Tests ─────────────────────────────────────────────┐  │
│ │ Name                  Status     Confidence  Duration           │  │
│ │ sensor-latency-001    RUNNING    87.3%       12h/24h           │  │
│ │ mem-optimization-003  RUNNING    42.1%        4h/48h           │  │
│ └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│ [Dashboard] [Build] [Deploy] [Agents] [Fleet] [Logs] [Settings]    │
│ 1           2       3        4        5       6       7             │
└──────────────────────────────────────────────────────────────────────┘
```

### Tab 2: Build

The Build tab provides an interactive build interface with real-time compiler output.

```
┌─ FLUX OS TUI ── Build ─────────────────────────────────────────────┐
│                                                                      │
│ ┌─ Build Configuration ──────────────────────────────────────────┐  │
│ │ Source:     [apps/greenhouse.flux.md                    ]     │  │
│ │ Target:     [arm64 (Raspberry Pi 4)                   ▼]     │  │
│ │ Optimize:   [-O2 (balanced)                           ▼]     │  │
│ │ Output:     [./build/greenhouse-monitor.fluxbc        ]     │  │
│ │ Board:      [rpi4                                     ▼]     │  │
│ │ Debug:      [ ] Include debug symbols                         │  │
│ └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│ ┌─ Compiler Output ─────────────────────────────────────────────┐  │
│ │ [00:00.012] Lexer:   tokenized 342 tokens from FLUX.MD      │  │
│ │ [00:00.034] Parser:  constructed AST (12 sections, 8 agents) │  │
│ │ [00:00.078] FIR:     generated 34 functions, 128 values      │  │
│ │ [00:00.089] FIR:     SSA validation passed                   │  │
│ │ [00:00.142] Opt:     dead code elimination: removed 12 vals  │  │
│ │ [00:00.156] Opt:     constant folding: 8 values folded       │  │
│ │ [00:00.201] Codegen: ARM64 assembly (4,812 instructions)     │  │
│ │ [00:00.287] Emit:    wrote 48,192 bytes to .fluxbc           │  │
│ │ [00:00.287] ✓ Build successful (287ms)                       │  │
│ └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│ ┌─ Build History ───────────────────────────────────────────────┐  │
│ │ Time      Target   Source          Size    Duration  Status   │  │
│ │ 14:23:01  arm64    greenhouse.md   48.2KB  287ms     ✓ OK    │  │
│ │ 14:15:22  arm64    greenhouse.md   47.8KB  291ms     ✓ OK    │  │
│ │ 13:45:00  bytecode factory.md     12.1KB  156ms     ✓ OK    │  │
│ └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│ [B] Build  [C] Cross-Compile  [D] Deploy  [T] Run Tests           │
└──────────────────────────────────────────────────────────────────────┘
```

**Build tab shortcuts:**
| Key | Action |
|-----|--------|
| `B` | Start build |
| `C` | Cross-compile (prompts for target) |
| `D` | Deploy current build |
| `T` | Run tests |
| `Ctrl+F` | Find in build output |
| `Ctrl+E` | Edit source file in $EDITOR |

### Tab 3: Deploy

```
┌─ FLUX OS TUI ── Deploy ───────────────────────────────────────────┐
│                                                                      │
│ ┌─ Deployment Config ────────────────────────────────────────────┐  │
│ │ Artifact: [build/greenhouse-monitor.fluxbc            ]     │  │
│ │ Target:   [fleet: greenhouse                         ▼]     │  │
│ │ Strategy: [rolling (25% batches)                     ▼]     │  │
│ │ Health:   [✓] Enable health checks                         │  │
│ │ Rollback: [✓] Rollback on failure                         │  │
│ └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│ ┌─ Deployment Progress ──────────────────────────────────────────┐  │
│ │ ████████████████████░░░░░░░░░░░░░░  60% (12/20 devices)       │  │
│ │                                                               │  │
│ │ Device              Status     Version    Duration             │  │
│ │ ● greenhouse-pi-01  UPDATED    v2.1.0     3.2s               │  │
│ │ ● greenhouse-pi-02  UPDATED    v2.1.0     3.1s               │  │
│ │ ● greenhouse-pi-03  UPDATED    v2.1.0     3.4s               │  │
│ │ ● greenhouse-pi-04  UPDATED    v2.1.0     2.9s               │  │
│ │ ◐ greenhouse-pi-05  DEPLOYING  v2.1.0     ...                │  │
│ │ ○ greenhouse-pi-06  PENDING    v2.0.3     -                  │  │
│ │ ○ greenhouse-pi-07  PENDING    v2.0.3     -                  │  │
│ └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│ [Enter] Deploy  [S] Stop  [R] Retry Failed  [H] View Health        │
└──────────────────────────────────────────────────────────────────────┘
```

### Tab 4: Agents

### Tab 5: Fleet

### Tab 6: Logs

### Tab 7: Settings

---

## Themes and Customization

The TUI supports color themes and configurable keybindings through the configuration file:

```toml
# ~/.flux/tui.toml

[theme]
name = "dark"
# Custom colors (ANSI 256-color)
colors = {
  title = "cyan"
  highlight = "yellow"
  success = "green"
  error = "red"
  warning = "magenta"
  dim = "gray"
}

[keybindings]
# Override default keybindings
quit = "q"
help = "?"
refresh = "r"
```
