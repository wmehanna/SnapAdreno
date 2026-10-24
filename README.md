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

Build profiles name the per-unit define sets of deployed kexts. `PROFILE=<name> ./build.sh` sources
`profiles/<name>.sh`, builds into `../out/<name>/` (or `OUT`) and writes the sets it used to `DEFINES.txt` there.
Without `PROFILE` the build is unchanged.

| Profile | Configuration |
| --- | --- |
| `desktop` | The A16 desktop kext with IO-coherent maintenance skipping (`ADRENO_COHERENT_SKIP_MAINT`). Needs a `--gpu-coherent` monitor and the boot-arg `adreno_coherent=1` to skip anything. |
| `desktop-nocoherent` | The same without coherency: sparse maintenance, the IB pipeline, GPU recovery, the present relatch, resident re-pin and aligned live-scanout copies. |

Built from the commit that added them, the profiles match their deployed kexts section for section (the umbrella's
`tools/macho-sections.py`). The re-pin log fix that followed changes `AdrenoMemoryMap.o` in both. The file bytes also
carry object paths, object times and a UUID; with `ZERO_AR_DATE=1` two builds in the same checkout are byte-identical.

Licence: the source here is MIT (see `LICENSE`). The build also compiles the umbrella's
`compat/adreno-ib-tables.h`, which is GPL-2.0-only, so a built kext is a combined work distributable under
GPL-2.0. See the umbrella's `LICENSE` and `THIRD_PARTY_NOTICES.md`.
