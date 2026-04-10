# FLUX OS — Edge IoT Deployment Guide

## Overview

FLUX OS is designed from the ground up for edge computing. The combination of a self-compiling kernel, hardware-agnostic HAL, and built-in fleet management means you can develop an application once on your laptop, cross-compile it for any target architecture, deploy it to hundreds of edge devices, and manage the entire lifecycle — builds, deployments, rollbacks, and A/B tests — from a single CLI, TUI, or Web interface.

This guide covers the complete edge IoT deployment workflow: preparing devices, building applications, deploying to fleets, monitoring health, collecting metrics, and performing hot-swaps. Whether you're managing a handful of Raspberry Pis in a greenhouse or thousands of ESP32s across a factory floor, the same tools and workflows apply.

---

## 1. Supported Hardware Platforms

FLUX OS runs on a wide range of hardware thanks to its pluggable HAL. The following platforms are supported or in active development:

| Platform | Architecture | HAL Backend | Status | Typical Use Case |
|----------|-------------|-------------|--------|-----------------|
| Raspberry Pi 4/5 | ARM64 | `hal_arm64` | Alpha | Sensor hubs, gateways |
| Raspberry Pi Zero 2 | ARM64 | `hal_arm64` | Planned | Low-cost sensors |
| ESP32-S3 | Xtensa | `hal_xtensa` | Planned | Ultra-low-cost IoT |
| NVIDIA Jetson | ARM64 | `hal_arm64` | Planned | Edge AI inference |
| BeagleBone Black | ARM32 | `hal_arm32` | Planned | Industrial control |
| RISC-V dev boards | RISC-V 64 | `hal_riscv64` | Alpha | Emerging IoT |
| x86_64 mini PCs | x86_64 | `hal_x86_64` | Alpha | Edge servers |
| Any Linux box | Native | `hal_native` | Stable | Development/testing |

### Hardware Requirements

**Minimum for any platform:**
- 16 MB RAM (FLUX OS kernel + VM + agent runtime)
- 1 MB flash/storage for FLUX OS image
- Network connectivity (Ethernet or WiFi) for fleet management
- UART or USB for initial flashing and debugging

**Recommended for production:**
- 64 MB+ RAM (allows multiple agents and larger bytecodes)
- 16 MB+ flash (FLUX OS + application bytecodes + logs)
- Watchdog timer (automatic reboot on hang)
- RTC or NTP (for timestamped logs and metrics)

---

## 2. Device Preparation

### 2.1 Flashing FLUX OS to a Device

The first step is getting the FLUX OS kernel onto your target device. The exact process depends on the platform, but the general workflow is the same:

```bash
# For Raspberry Pi (SD card)
flux flash --device /dev/sdX --target arm64 --board rpi4

# For any platform via UART/USB
flux flash --serial /dev/ttyUSB0 --baud 1152000 --target arm64

# For network boot (PXE)
flux flash --network --mac AA:BB:CC:DD:EE:FF --tftp-server 192.168.1.100
```

The flash command writes the FLUX OS kernel image to the device's storage, configures the bootloader to load FLUX OS at boot, and optionally writes initial configuration (network settings, fleet ID, device name).

### 2.2 Initial Configuration

After flashing, you need to configure the device's identity and network settings. This can be done via a serial console during first boot or through a configuration file:

```bash
# Configure device identity
flux device configure --serial /dev/ttyUSB0 \
  --name greenhouse-pi-01 \
  --fleet greenhouse \
  --wifi-ssid "MyNetwork" \
  --wifi-password "MyPassword" \
  --ntp-server pool.ntp.org
```

Alternatively, create a `flux-config.toml` file on the boot partition:

```toml
[device]
name = "greenhouse-pi-01"
fleet = "greenhouse"

[network]
type = "wifi"
ssid = "MyNetwork"
password = "MyPassword"
dhcp = true

[time]
ntp_server = "pool.ntp.org"
timezone = "UTC"

[agent]
heartbeat_interval_ms = 30000
log_level = "info"
```

### 2.3 Device Registration

Once the device boots and connects to the network, register it with the fleet manager:

```bash
# Register a device (prompts for confirmation)
flux device register --name greenhouse-pi-01 --fleet greenhouse

# Register with auto-confirm (for scripted setups)
flux device register --name greenhouse-pi-01 --fleet greenhouse --auto-confirm

# Bulk-register multiple devices from a file
flux device register --batch devices.csv
```

The CSV format for bulk registration:
```csv
name,fleet,arch,board,endpoint
greenhouse-pi-01,greenhouse,arm64,rpi4,192.168.1.101
greenhouse-pi-02,greenhouse,arm64,rpi4,192.168.1.102
greenhouse-pi-03,greenhouse,arm64,rpi4,192.168.1.103
factory-esp-01,factory,xtensa,esp32,192.168.1.201
```

### 2.4 Verifying Device Health

After registration, verify the device is online and healthy:

```bash
# List all devices
flux device list

# Detailed status of a specific device
flux device status --name greenhouse-pi-01

# Health check (exit code: 0=healthy, 1=degraded, 2=unhealthy, 3=unreachable)
flux device health --name greenhouse-pi-01 --quiet

# Fleet-wide summary
flux fleet summary --name greenhouse
```

---

## 3. Building for Edge Targets

### 3.1 Cross-Compilation

FLUX OS's self-compiler can target any architecture. When you build from your development machine (which might be x86_64), you specify the target architecture and the compiler generates optimized bytecode or native code for that target:

```bash
# Build for ARM64 (Raspberry Pi)
flux build --source apps/temperature.flux.md --target arm64 --board rpi4 --optimize -Os

# Build for RISC-V
flux build --source apps/temperature.flux.md --target riscv64 --optimize -O2

# Build portable bytecode (runs on any FLUX VM regardless of host arch)
flux build --source apps/temperature.flux.md --target bytecode --optimize -O1

# Build for WASM (runs in browser or edge WASM runtime)
flux build --source apps/temperature.flux.md --target wasm32 --optimize -Os
```

### 3.2 Optimization Levels

| Level | Flag | Description | Best For |
|-------|------|-------------|----------|
| None | `-O0` | No optimization, fastest compile | Debugging |
| Size | `-Os` | Optimize for binary size | Flash-constrained devices |
| Speed | `-O2` | Balanced speed optimization | General use |
| Aggressive | `-O3` | Maximum speed, may increase size | Performance-critical paths |

### 3.3 Board-Specific Optimization

When you specify a `--board` flag, the compiler uses board-specific knowledge to generate optimal code. For example, a Raspberry Pi 4 has ARM Cortex-A72 cores with NEON SIMD, while a Raspberry Pi Zero has ARM Cortex-A53 cores. The compiler generates different code for each:

```bash
# Board-specific compilation
flux build --target arm64 --board rpi4    # Cortex-A72, NEON
flux build --target arm64 --board rpi-zero # Cortex-A53, NEON
flux build --target arm64 --board jetson-nano # Cortex-A57, NEON + GPU hints
```

---

## 4. Deployment Strategies

### 4.1 Deployment Overview

FLUX OS supports multiple deployment strategies, each suitable for different scenarios. All strategies support automatic rollback on failure.

```bash
# Rolling deployment (update N devices at a time)
flux deploy --fleet greenhouse --strategy rolling --batch-size 25%

# Canary deployment (update 1 device, wait, then continue)
flux deploy --fleet greenhouse --strategy canary --canary-count 1 --canary-wait 10m

# Blue-green deployment (update all devices, then switch traffic)
flux deploy --fleet greenhouse --strategy blue-green

# All-at-once (update everything simultaneously — fastest but riskiest)
flux deploy --fleet greenhouse --strategy all-at-once
```

### 4.2 Strategy Comparison

| Strategy | Speed | Risk | Rollback Time | Best For |
|----------|-------|------|---------------|----------|
| Rolling | Medium | Low | Minutes | Production fleets |
| Canary | Slow | Very Low | Minutes | Critical systems |
| Blue-Green | Fast | Low | Seconds | High-availability systems |
| All-at-once | Fastest | Highest | Minutes | Dev/test environments |

### 4.3 Monitoring Deployments

```bash
# Watch deployment progress in real-time
flux deploy status --follow

# Get detailed deployment report
flux deploy report --id deploy-12345

# Check deployment history
flux deploy history --fleet greenhouse --limit 10
```

### 4.4 Automatic Rollback

Deployments automatically roll back if health checks fail within the grace period:

```bash
# Deploy with automatic rollback on failure
flux deploy --fleet greenhouse --strategy rolling \
  --health-check-interval 30s \
  --health-check-timeout 5m \
  --rollback-on-failure

# Manual rollback
flux deploy rollback --fleet greenhouse --to-version 1.5.0
```

---

## 5. Fleet Management

### 5.1 Fleet Organization

Devices are organized into fleets. A fleet is a logical group of devices that share the same application and deployment configuration. You can have multiple fleets, and devices can be moved between fleets:

```bash
# Create a fleet
flux fleet create --name greenhouse --description "Greenhouse sensor array"

# List fleets
flux fleet list

# Add a device to a fleet
flux fleet add-device --fleet greenhouse --device greenhouse-pi-01

# Move a device between fleets
flux fleet move-device --device greenhouse-pi-03 --from greenhouse --from-hydroponics

# Remove a device from a fleet
flux fleet remove-device --fleet greenhouse --device greenhouse-pi-05
```

### 5.2 Fleet-Wide Operations

```bash
# Deploy to entire fleet
flux deploy --fleet greenhouse --artifact build/app-v2.fluxbc --strategy rolling

# Fleet-wide health check
flux fleet health --name greenhouse

# Fleet-wide restart (reboot FLUX OS on all devices)
flux fleet restart --name greenhouse --strategy rolling --batch-size 10%

# Fleet-wide command execution (run a command on all devices)
flux fleet exec --name greenhouse --command "flux_log_level debug"

# Collect fleet-wide logs
flux fleet logs --name greenhouse --since 1h --level warn
```

### 5.3 Fleet Metrics

```bash
# Real-time fleet metrics
flux fleet metrics --name greenhouse --interval 5s

# Historical metrics
flux fleet metrics --name greenhouse --since 24h --aggregate avg

# Specific metric
flux fleet metrics --name greenhouse --metric memory_usage --format json
```

---

## 6. Device Monitoring

### 6.1 Real-Time Logs

```bash
# Stream logs from a single device
flux device logs --name greenhouse-pi-01 --follow

# Stream logs from entire fleet
flux fleet logs --name greenhouse --follow

# Filter logs by level
flux device logs --name greenhouse-pi-01 --level error

# Search logs for a pattern
flux device logs --name greenhouse-pi-01 --grep "temperature"

# Export logs to a file
flux fleet logs --name greenhouse --since 24h --output greenhouse-logs.jsonl
```

### 6.2 Health Checks

The health check system monitors device health at multiple levels:

| Check | Description | Frequency |
|-------|-------------|-----------|
| Heartbeat | Device is responsive | Every 30s (configurable) |
| Process | Agent processes are running | Every 60s |
| Memory | Free memory above threshold | Every 60s |
| Network | Device can reach fleet manager | Every 30s |
| Application | Application-specific health | Custom interval |

```bash
# Run health check
flux device health --name greenhouse-pi-01

# Set up alerting on health failures
flux alert create --name greenhouse-down \
  --condition "fleet:greenhouse device:health != healthy" \
  --action notify --channel slack
```

### 6.3 Device Shell

For debugging, you can open a remote shell on a device:

```bash
# Open interactive shell
flux device shell --name greenhouse-pi-01

# Run a single command
flux device exec --name greenhouse-pi-01 -- "flux_kernel_info"

# Copy files to/from device
flux device cp --to greenhouse-pi-01 --path /local/app.fluxbc --dest /flux/apps/
flux device cp --from greenhouse-pi-01 --path /flux/logs/ --dest /local/logs/
```

---

## 7. Network Architecture

### 7.1 Device Communication

```
┌─────────────────────────────────────────────────────────┐
│                    Development Machine                    │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│  │  CLI      │  │  TUI     │  │  Web     │              │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘              │
│       └──────────────┼──────────────┘                     │
│                      │ gRPC/HTTP                          │
├──────────────────────┼───────────────────────────────────┤
│                      │                                    │
│           ┌──────────▼──────────┐                         │
│           │   Fleet Manager     │                         │
│           │   (orchestrator)    │                         │
│           └──────────┬──────────┘                         │
│                      │                                    │
│       ┌──────────────┼──────────────┐                     │
│       │              │              │                     │
│  ┌────▼────┐   ┌────▼────┐   ┌────▼────┐               │
│  │ Device 1│   │ Device 2│   │ Device N│               │
│  │ (FLUX)  │   │ (FLUX)  │   │ (FLUX)  │               │
│  └─────────┘   └─────────┘   └─────────┘               │
│       │              │              │                     │
│  ┌────▼────┐   ┌────▼────┐   ┌────▼────┐               │
│  │ Sensors │   │ Sensors │   │ Sensors │               │
│  └─────────┘   └─────────┘   └─────────┘               │
└──────────────────────────────────────────────────────────┘
```

### 7.2 Communication Protocols

| Protocol | Port | Purpose |
|----------|------|---------|
| gRPC | 9090 | Device-to-manager communication |
| HTTP | 8080 | Web dashboard and REST API |
| MQTT | 1883 | Lightweight device messaging (optional) |
| CoAP | 5683 | Constrained device messaging (optional) |
| mDNS | 5353 | Local device discovery |
| SSE | 8080 | Server-sent events for log streaming |

---

## 8. Security Model

### 8.1 Device Authentication

Each device has a unique agent ID and capability token. These are generated during device registration and stored in secure storage on the device. All communications between devices and the fleet manager are authenticated using these tokens.

### 8.2 Transport Encryption

All device-to-manager communications use TLS 1.3 by default. For constrained devices that cannot support TLS, a lightweight encryption scheme based on ChaCha20-Poly1305 is available.

### 8.3 Capability Enforcement

Each device runs agents under the capability security model. Even if a device is compromised, the attacker cannot escalate privileges beyond the agent's granted capabilities. The kernel enforces capability checks on every syscall.

---

## 9. Troubleshooting

### Common Issues

**Device won't connect to network:**
```bash
# Check if device is reachable
ping greenhouse-pi-01

# Check device serial output for boot errors
flux device serial --name greenhouse-pi-01 --follow

# Reconfigure network
flux device configure --name greenhouse-pi-01 --wifi-ssid "NewNetwork" --wifi-password "NewPassword"
```

**Deployment fails on some devices:**
```bash
# Check deployment status for failures
flux deploy status --show-failures

# Get device-specific error details
flux device logs --name greenhouse-pi-03 --since 10m --level error

# Retry failed devices
flux deploy retry --id deploy-12345 --devices greenhouse-pi-03,greenhouse-pi-07
```

**Device runs out of memory:**
```bash
# Check memory usage
flux device health --name greenhouse-pi-01 --verbose

# Reduce agent memory limits
flux agent configure --device greenhouse-pi-01 --agent temp-monitor --max-memory 512KB

# Restart device to reclaim leaked memory
flux device restart --name greenhouse-pi-01
```
