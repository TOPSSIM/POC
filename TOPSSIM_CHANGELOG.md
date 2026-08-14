# TOPSSIM PoC Change Log

This file records TOPSSIM proof-of-concept changes in enough detail for another
agent to reconstruct the implementation from the source files plus this log.

## 2026-07-10 - Initial PoC Skeleton

Status: in progress

Goal:
- Add a minimal TOPSSIM proof-of-concept beside the existing Open5GS test app
  workflow.
- Preserve the existing `tests/app/5gc` behavior.
- Add a new `tests/app/TOPSSIM` executable that starts Open5GS 5GC components
  and a Python TOPSSIM sidecar.

Planned files:
- `topssim/poc/sidecar.py`: dependency-free Python HTTP sidecar.
- `topssim/tools/trigger_prediction.py`: CLI helper that sends a manual MPF
  trigger to the sidecar.
- `tests/app/topssim-init.c`: Open5GS test-app initializer for `TOPSSIM`.
- `tests/app/meson.build`: Meson wiring for the `TOPSSIM` executable.
- `tests/app/test-app.h`: declaration for TOPSSIM init/final helpers.

Validation:
- Pending.
