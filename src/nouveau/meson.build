# Copyright © 2021 Ilia Mirkin
# SPDX-License-Identifier: MIT

subdir('headers')
subdir('drm')
subdir('winsys')
if with_nouveau_vk
  subdir('rust')
  subdir('nil')
  subdir('compiler')
endif
if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif

if with_nouveau_vk
  if host_machine.system() != 'horizon'
    subdir('cubin')
  endif
  subdir('mme')
  subdir('vulkan')
endif
