# FLUX OS

Microkernel operating system in pure C11 where the kernel IS the compiler. Hardware-agnostic, self-compiling, and agent-native.

## Brand Line
> An OS that writes its own code — FLUX OS compiles FLUX.MD to native binaries at boot.

## Installation

```bash
git clone https://github.com/SuperInstance/flux-os.git
cd flux-os
make
```

```bash
# Run hosted-mode tests
make test
```

## Usage

```bash
# Build for current host
flux build --target native

# Cross-compile for ARM64
flux build --target arm64 --board rpi4

# Deploy to edge fleet
flux deploy --fleet greenhouse-sensors --strategy canary
```

## Fleet Context

Part of the Cocapn fleet. Related repos:
- [flux](https://github.com/SuperInstance/flux) — Rust production runtime with 64-register VM
- [flux-runtime](https://github.com/SuperInstance/flux-runtime) — Python reference implementation for research
- [flux-runtime-c](https://github.com/SuperInstance/flux-runtime-c) — C port of the FLUX runtime

---
🦐 Cocapn fleet — lighthouse keeper architecture