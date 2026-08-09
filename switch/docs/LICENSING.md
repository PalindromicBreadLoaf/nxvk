# Licensing

This tree is a mixed-licence aggregate. Consult each file's SPDX identifier for its terms.

## The two halves

Upstream Mesa keeps its own licences. Their notices must be kept intact in any redistribution.

Any files added by this fork are `GPL-2.0-or-later`. Every one carries the SPDX tag and the
copyright line from `LICENSE_HEADER.txt`:

- `src/nouveau/vulkan/nvkmd/nvgpu/`
- `src/vulkan/wsi/wsi_switch.c`
- `src/egl/drivers/horizon/egl_horizon.c`
- `src/util/disk_cache_horizon.{c,h}`
- `switch/**`

Small edits to existing upstream files keep their original licensing.

## What this means for homebrew linking the driver

Horizon has no shared-library mechanism. `libnvk.a` is linked statically into each `.nro`, so an
application that links it forms a combined work covered by the GPL. Put simply:

- **You may distribute binaries.** Publishing the `.nro` on a release page alongside the
  corresponding source satisfies the GPLv2 license.
- **Your application source must be available** under GPL-compatible terms to anyone who
  receives the binary. Permissively licensed ports (MIT, BSD, Apache-2.0, etc.) are fine since those
  licences are GPL-compatible. The GPL license only covers this driver and the code within.
- **Closed-source applications cannot be distributed with this driver.** Bulding something privately
  just for yourself is fine, but you cannot distribute any closed source code using this driver.
- "Corresponding source" for the driver means this repository at the revision you built from,
  including any modifications you made to it.

## Contributing back to Mesa

A change to a file that carries an upstream MIT header may be sent upstream as MIT, as normal.
Code in the fork-owned files listed above is GPL and cannot be, so keep genuinely
upstreamable work in upstream files rather than pulling it into the Horizon backends.

Do not paste source from other Switch driver projects into this tree, regardless of their
licence.
