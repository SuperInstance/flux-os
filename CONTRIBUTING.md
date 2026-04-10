# Contributing to FLUX OS

Thank you for your interest in contributing to FLUX OS! This guide covers everything you need to know to contribute effectively, whether you want to fix a bug, add a feature, improve documentation, or contribute a new HAL backend.

---

## Development Environment

### Prerequisites

- GCC 7+ or Clang 6+ with C11 support
- GNU Make 4.0+
- Git
- (Optional) QEMU for bare-metal testing
- (Optional) valgrind for memory debugging
- (Optional) clang-format for consistent code style

### Building

```bash
git clone https://github.com/SuperInstance/flux-os.git
cd flux-os
make clean && make
make test
```

### Code Style

FLUX OS follows a consistent C coding style:

- **Indentation**: 4 spaces, no tabs
- **Line length**: 100 characters maximum
- **Naming**: `snake_case` for functions and variables, `FLUX_PREFIX_UPPER_CASE` for macros and types
- **Headers**: Every .c file starts with a block comment describing the module's purpose
- **Comments**: Use `/* */` for block comments, prefer `//` for inline explanations
- **Functions**: One function per logical operation, max 50 lines per function
- **Error handling**: Always check return values, use flux_status_t for error propagation

Run the formatter before committing:
```bash
clang-format -i src/*.c include/flux/*.h
```

---

## Project Structure

| Directory | Purpose |
|-----------|---------|
| `include/flux/` | Public API headers (this is the stable interface) |
| `kernel/` | Microkernel implementation |
| `vm/` | Bytecode virtual machine |
| `fluxc/` | Self-compiler (lexer, parser, FIR) |
| `hal/` | Hardware abstraction layer |
| `hal/arch/` | Architecture-specific HAL backends |
| `agent/` | Agent runtime (A2A, capabilities, sandboxing) |
| `tests/` | Test suites |
| `docs/` | Documentation |

---

## Contribution Workflow

### 1. Fork and Branch

```bash
git clone https://github.com/YOUR_USERNAME/flux-os.git
cd flux-os
git checkout -b feature/my-feature
```

### 2. Make Changes

- Write code following the style guide above
- Add tests for new functionality
- Update documentation if changing behavior

### 3. Test

```bash
make clean && make        # Ensure it builds
make test                 # Ensure all tests pass
```

### 4. Commit

Use clear, descriptive commit messages:
```
feat(vm): add SIMD vector region support
fix(hal): correct ARM64 timer frequency calculation
docs: update onboarding guide for new deploy command
refactor(agent): simplify capability check logic
test(vm): add region boundary tests
```

### 5. Push and PR

```bash
git push origin feature/my-feature
# Open a pull request on GitHub
```

---

## Areas for Contribution

### High Priority
- **HAL Backends**: ARM64 bare-metal, RISC-V bare-metal, ESP32 (Xtensa)
- **Compiler**: Complete FLUX.MD parser, FIR optimization passes, native codegen backends
- **Tests**: More VM tests, agent runtime tests, compiler tests
- **Documentation**: Examples, tutorials, troubleshooting guides

### Medium Priority
- **CLI Tool**: Implement the `flux` CLI (currently in design/spec phase)
- **TUI**: Implement the terminal user interface
- **Web Interface**: Implement the web dashboard and REST API
- **Fleet Manager**: Implement device registration, deployment, and health checking

### Low Priority (Advanced)
- **New Opcodes**: Propose new instructions for the FLUX ISA
- **FIR Extensions**: New types, optimization passes, or codegen backends
- **Porting**: FreeBSD, OpenBSD, Windows (WSL), other platforms
- **Formal Verification**: Prove correctness of VM, scheduler, or capability model

---

## Adding a HAL Backend

To add support for a new architecture, create a new HAL backend:

1. Create `hal/arch/<arch>/hal_<arch>.c`
2. Implement all functions in the `flux_hal_t` vtable
3. Create `hal/arch/<arch>/` with architecture-specific boot code
4. Register the backend in `hal/hal.c`
5. Add architecture detection in `hal/hal.c`
6. Add tests for the new backend in `tests/`

See `hal/arch/native/hal_native.c` as a reference implementation.

---

## Adding an Opcode

To add a new instruction to the FLUX ISA:

1. Add the opcode define to `include/flux/opcodes.h`
2. Add the opcode name to `vm/opcodes.c` (`flux_opcode_name()`)
3. Implement execution logic in `vm/vm.c` (`flux_vm_step()`)
4. Add FIR support in `include/flux/compiler.h` and `fluxc/fir.c`
5. Add tests in `tests/test_hosted.c`
6. Update `FLUX_OPCODE_COUNT` in `include/flux/opcodes.h`
7. Update documentation

---

## Reporting Issues

When reporting bugs, please include:

1. **FLUX OS version** (`flux --version` or `FLUX_OS_VERSION_STRING`)
2. **Host OS and architecture** (e.g., "Ubuntu 22.04, x86_64")
3. **Target** (if cross-compiling)
4. **Steps to reproduce**
5. **Expected behavior**
6. **Actual behavior**
7. **Relevant log output**

For feature requests, describe the use case, the proposed behavior, and any alternative approaches you've considered.

---

## Code of Conduct

- Be respectful and constructive in all interactions
- Focus on the technical merits of proposals
- Welcome newcomers and help them get started
- Write clear documentation and comments
- Test your changes thoroughly before submitting
