# FLUX OS — Web Interface Guide

## Overview

The FLUX OS Web interface is a browser-based dashboard for fleet operations, monitoring, and non-developer management of FLUX OS devices and applications. It runs as a lightweight HTTP server embedded directly in FLUX OS — no separate web server, no database, no external dependencies. You open a browser, navigate to the device running FLUX OS, and you have a full management dashboard.

The Web interface is designed for three primary audiences:

1. **Fleet Operators** who need to monitor device health, deploy updates, and manage A/B tests across hundreds or thousands of devices
2. **Non-Developers** (e.g., greenhouse managers, factory supervisors) who need to see application status without using a terminal
3. **Monitoring Systems** that consume the REST API for integration with external dashboards, alerting systems, and data pipelines

---

## Starting the Web Interface

```bash
# Start on localhost, port 8080
flux web

# Start on all interfaces (accessible from network)
flux web --bind 0.0.0.0 --port 8080

# Start with authentication
flux web --auth token --token-file ~/.flux/web-token

# Start with TLS
flux web --tls --cert /path/to/cert.pem --key /path/to/key.pem

# Start in background
flux web --daemon
```

Then open `http://localhost:8080` (or `https://` if using TLS) in your browser.

---

## Dashboard

The main dashboard provides an at-a-glance overview of your FLUX OS environment:

### System Overview Cards
- **Total Devices**: Number of registered devices, broken down by status (online, offline, error)
- **Active Agents**: Number of running agents across all devices
- **Avg Build Time**: Average compilation time across recent builds
- **System Uptime**: How long the fleet manager has been running
- **Active A/B Tests**: Number of currently running tests with their confidence levels

### Fleet Health Grid
A visual grid showing the health status of all devices, color-coded:
- 🟢 Green: Healthy
- 🟡 Yellow: Degraded (warnings)
- 🔴 Red: Unhealthy (errors)
- ⚫ Gray: Offline

### Recent Activity Feed
A real-time feed of recent events:
- Deployments (started, completed, failed, rolled back)
- A/B test events (created, promoted, rolled back)
- Device events (online, offline, health alerts)
- Agent events (started, stopped, crashed)
- Compilation events (succeeded, failed)

### Active A/B Tests Summary
A table of all running A/B tests with:
- Test name and status
- Variant labels and device allocation
- Key metrics comparison
- Confidence level progress bar
- Quick actions (promote, extend, stop, rollback)

---

## Deployment Management

### Create Deployment
A guided deployment wizard:
1. **Select artifact**: Upload a .fluxbc file or select from recent builds
2. **Select target**: Choose a device or fleet
3. **Choose strategy**: Rolling, canary, blue-green, or all-at-once
4. **Configure options**: Batch size, health checks, rollback policy
5. **Review and confirm**: Shows summary before deploying

### Deployment Progress
Real-time progress view showing:
- Progress bar with device count
- Per-device status table (device name, status, version, duration)
- Live log output from deploying devices
- Ability to stop or retry failed devices

### Deployment History
Historical view of all deployments with filtering by:
- Date range
- Fleet or device
- Status (success, failure, rolled back)
- Version

---

## A/B Testing Interface

### Test Creation
A form-based interface for creating A/B tests:
- Upload or select variant artifacts
- Configure split ratio with a slider
- Select metrics to track (checkboxes)
- Set duration, significance threshold, and auto-actions
- Target fleet or device group

### Test Dashboard
Visual comparison of variant performance:
- Side-by-side metric cards with sparkline charts
- Statistical significance progress bar
- Effect size indicators (percentage improvement/regression)
- Device allocation pie chart
- Quick action buttons (promote, extend, stop, rollback)

### Results Analysis
Detailed statistical analysis with:
- Metric comparison tables with p-values and confidence intervals
- Distribution charts (histograms, box plots)
- Time-series charts showing metric evolution over test duration
- Cohort analysis (per-device or per-group breakdowns)

---

## Device Management

### Device List
A filterable, sortable table of all registered devices:
- Columns: Name, Fleet, Status, Version, Uptime, Memory, CPU, Last Heartbeat
- Filters: Fleet, status, board type, version
- Actions: View details, deploy, restart, shell, remove

### Device Detail Page
Comprehensive device information:
- **Info tab**: Hardware details, firmware version, network configuration
- **Agents tab**: List of running agents with their status, capabilities, and resource usage
- **Logs tab**: Real-time log viewer with level filtering and search
- **Metrics tab**: Time-series charts for all collected metrics
- **Deploy tab**: Deployment history for this device
- **Config tab**: Device configuration (editable, with save/apply)

---

## REST API

The Web interface is built on top of a REST API that is available for programmatic access. All endpoints return JSON and support CORS for browser-based integration.

### Authentication

```bash
# Token-based authentication
curl -H "Authorization: Bearer <token>" https://flux.local:8080/api/v1/status
```

### Endpoints

#### System
```
GET  /api/v1/status              System status and version info
GET  /api/v1/hardware            Hardware capabilities (from HAL)
GET  /api/v1/config              Current configuration
PUT  /api/v1/config              Update configuration
```

#### Devices
```
GET    /api/v1/devices                     List all devices
GET    /api/v1/devices/:id                 Device details
POST   /api/v1/devices/:id/deploy          Deploy to device
POST   /api/v1/devices/:id/hotswap         Hot-swap bytecode
DELETE /api/v1/devices/:id                 Remove device
GET    /api/v1/devices/:id/health          Health check
GET    /api/v1/devices/:id/logs            Device logs
POST   /api/v1/devices/:id/exec            Execute command
GET    /api/v1/devices/:id/metrics         Device metrics
```

#### Fleets
```
GET    /api/v1/fleets                      List fleets
POST   /api/v1/fleets                      Create fleet
GET    /api/v1/fleets/:id                  Fleet details
DELETE /api/v1/fleets/:id                  Delete fleet
POST   /api/v1/fleets/:id/deploy           Deploy to fleet
GET    /api/v1/fleets/:id/health           Fleet health
GET    /api/v1/fleets/:id/metrics          Fleet metrics
POST   /api/v1/fleets/:id/exec             Execute on fleet
GET    /api/v1/fleets/:id/logs             Fleet logs
```

#### A/B Tests
```
GET    /api/v1/ab-tests                    List A/B tests
POST   /api/v1/ab-tests                    Create A/B test
GET    /api/v1/ab-tests/:id                Test details and results
PUT    /api/v1/ab-tests/:id                Update test parameters
POST   /api/v1/ab-tests/:id/promote        Promote variant
POST   /api/v1/ab-tests/:id/rollback       Rollback test
POST   /api/v1/ab-tests/:id/stop           Stop test
GET    /api/v1/ab-tests/:id/metrics        Test metrics (time-series)
GET    /api/v1/ab-tests/:id/export         Export test data (CSV/JSON)
```

#### Compilation
```
POST   /api/v1/compile                     Compile FLUX.MD
GET    /api/v1/compile/:id/status          Compilation status
GET    /api/v1/compile/:id/output          Compilation output
GET    /api/v1/compile/:id/download        Download compiled artifact
```

#### Agents
```
GET    /api/v1/agents                      List agents
GET    /api/v1/agents/:id                  Agent details
GET    /api/v1/agents/:id/health           Agent health
POST   /api/v1/agents/:id/restart          Restart agent
GET    /api/v1/agents/:id/a2a              A2A message history
```

#### Logs
```
GET  /api/v1/logs                          Log stream (SSE endpoint)
GET  /api/v1/logs?device=NAME              Device-specific logs (SSE)
GET  /api/v1/logs?fleet=NAME               Fleet logs (SSE)
GET  /api/v1/logs?level=error              Filtered logs (SSE)
```

### Example API Usage

```bash
# Get system status
curl -s https://flux.local:8080/api/v1/status | jq .

# List devices
curl -s -H "Authorization: Bearer $TOKEN" \
  https://flux.local:8080/api/v1/devices | jq '.devices[] | {name, status, fleet}'

# Deploy to fleet
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -F "artifact=@build/app.fluxbc" \
  -F "strategy=rolling" \
  -F "batch_size=25%" \
  https://flux.local:8080/api/v1/fleets/greenhouse/deploy

# Create A/B test
curl -X POST -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  -d '{
    "name": "latency-test-001",
    "variant_a": {"file": "v1.fluxbc"},
    "variant_b": {"file": "v2.fluxbc"},
    "split": "50/50",
    "fleet": "greenhouse",
    "duration": "24h",
    "metrics": ["latency_p99", "error_rate", "memory_bytes"]
  }' \
  https://flux.local:8080/api/v1/ab-tests

# Stream logs (Server-Sent Events)
curl -N https://flux.local:8080/api/v1/logs?level=error
```

### SSE Event Format

The log streaming endpoint uses Server-Sent Events:

```
event: log
data: {"device":"greenhouse-pi-01","level":"error","message":"temperature sensor timeout","timestamp":1735689600000}

event: device_status
data: {"device":"greenhouse-pi-03","old_status":"online","new_status":"offline","timestamp":1735689601000}

event: metric
data: {"device":"greenhouse-pi-01","metric":"latency_p99","value":8.2,"unit":"ms","timestamp":1735689602000}
```

---

## WebSocket API

For real-time bidirectional communication, the Web interface provides a WebSocket endpoint:

```
ws://flux.local:8080/api/v1/ws
```

WebSocket messages are JSON-encoded:

```json
// Subscribe to fleet events
{"action": "subscribe", "fleet": "greenhouse"}

// Server pushes events
{"type": "deploy_progress", "device": "greenhouse-pi-01", "progress": 0.75}

// Unsubscribe
{"action": "unsubscribe", "fleet": "greenhouse"}
```

---

## Embedding and Integration

The Web interface can be embedded in existing dashboards:

```html
<!-- Embed FLUX OS dashboard in an iframe -->
<iframe src="https://flux.local:8080/embed?fleet=greenhouse&hide-nav=true"
        width="100%" height="600" frameborder="0"></iframe>
```

For external dashboard integration (Grafana, Datadog, etc.), use the REST API to export metrics in Prometheus-compatible format:

```
GET /api/v1/metrics/prometheus
```

This endpoint returns metrics in Prometheus exposition format, making it easy to scrape with any Prometheus-compatible monitoring system.
