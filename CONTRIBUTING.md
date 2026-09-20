# Contributing

Thank you for improving LiteMon.

## Before opening a change

- Keep the project local-first and lightweight.
- Avoid adding daemons, cloud dependencies or large frameworks for convenience.
- Hardware-specific code must degrade gracefully when the hardware or tool is absent.
- Never synthesize a hardware metric that is not actually available.

## Build

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run formatting before submitting:

```bash
find src tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i
```

## Pull requests

Include:

- problem statement
- implementation summary
- user-visible behavior
- test coverage
- hardware/driver details for GPU changes
- screenshots only when UI behavior changed

New collectors should include fixture-driven parser tests whenever possible so CI does not depend on physical hardware.
