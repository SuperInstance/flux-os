# FLUX OS — 5-Minute Quickstart

Get FLUX OS running on your machine in under 5 minutes. This guide covers all three interfaces: CLI, TUI, and Web.

---

## Prerequisites Check (30 seconds)

```bash
# Check C compiler
gcc --version    # or: clang --version

# Check make
make --version

# Check git
git --version
```

If any of these are missing, install them with your package manager (apt, brew, pacman, etc.).

---

## Step 1: Clone and Build (60 seconds)

```bash
git clone https://github.com/SuperInstance/flux-os.git
cd flux-os

make
make test
```

You should see all tests pass. That's it — FLUX OS is running in hosted mode on your machine.

---

## Step 2: CLI Quickstart (60 seconds)

```bash
# See what commands are available
flux --help

# Build for current architecture
flux build

# Build for ARM64 (Raspberry Pi)
flux build --target arm64 --optimize -Os

# See the system info
flux info

# List connected devices (empty for now)
flux device list
```

---

## Step 3: TUI Quickstart (60 seconds)

```bash
# Launch the TUI
flux tui

# Inside the TUI:
# - Press ? for help
# - Press 1-7 to switch tabs
# - Press q to quit
# - Press / to search
# - Press r to refresh
```

---

## Step 4: Web Interface Quickstart (60 seconds)

```bash
# Start the web interface
flux web --port 8080

# Open in browser
# http://localhost:8080
```

In your browser you'll see the FLUX OS dashboard with system status, device list, and deployment controls.

---

## Next Steps

After the 5-minute quickstart, explore these guides:

| What to do next | Guide |
|----------------|-------|
| Full human onboarding | [docs/ONBOARDING.md](ONBOARDING.md) |
| Agent-first onboarding | [docs/AGENT-ONBOARDING.md](AGENT-ONBOARDING.md) |
| Architecture deep-dive | [docs/ARCHITECTURE.md](ARCHITECTURE.md) |
| All CLI commands | [docs/CLI-REFERENCE.md](CLI-REFERENCE.md) |
| Deploy to edge devices | [docs/EDGE-IOT-DEPLOYMENT.md](EDGE-IOT-DEPLOYMENT.md) |
| Hot-swap and A/B testing | [docs/HOTSWAP-AB-TESTING.md](HOTSWAP-AB-TESTING.md) |
| TUI keyboard shortcuts | [docs/TUI-GUIDE.md](TUI-GUIDE.md) |
| Web API reference | [docs/WEB-INTERFACE.md](WEB-INTERFACE.md) |
| Contribute to FLUX OS | [CONTRIBUTING.md](CONTRIBUTING.md) |
