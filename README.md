# SnapAdreno

The Adreno X2-90 GPU kernel driver for [Snapintosh](https://github.com/Snapintosh/Snapintosh). It contains:
- `AdrenoX2`: an IOGPU accelerator. It handles command submission, resource residency, the submission pipeline
  and GPU recovery.
- `AdrenoDisplay`: an IOMobileFramebuffer display with beam-synchronised, tear-free presentation.

Together with the Metal plug-in ([SnapMetal](https://github.com/Snapintosh/SnapMetal)) and the shader compiler
([air2ir3](https://github.com/Snapintosh/air2ir3)), it gives macOS hardware-accelerated graphics on the
Snapdragon X2 Elite.

This repository is mounted in the Snapintosh umbrella at `gpu/adreno-kext/kext` and builds from there with
`./build.sh`:
- It compiles shared code from the umbrella's `compat/` and includes `gpu/adreno-kext/include`.
- The `include/IOGPUFamily26A428.h` and `IOMobileGraphicsFamily26A428.h` headers are reconstructed from Apple's KDK
  and are **not distributed**. Generate them as described in the umbrella's `AGENTS.md` and `docs/BUILDING.md`.
- Most features are compile-time options (`ADRENO_*` defines) and are off by default.

Licence: the source here is MIT (see `LICENSE`). The build also compiles the umbrella's
`compat/adreno-ib-tables.h`, which is GPL-2.0-only, so a built kext is a combined work distributable under
GPL-2.0. See the umbrella's `LICENSE` and `THIRD_PARTY_NOTICES.md`.
