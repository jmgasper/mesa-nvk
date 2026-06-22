# Copyright © 2022 Collabora Ltd. and Red Hat Inc.
# SPDX-License-Identifier: MIT

# NVIDIA RM (nvrm) nvkmd backend, built as an internal static library.
# Talks to the NVIDIA kernel driver (e.g. /dev/graphics/nvidiaN on Haiku)
# rather than to DRM/libdrm.

nvkmd_nvrm_files = files(
  'nvkmd_nvrm.h',
  'nvkmd_nvrm_ctx.c',
  'nvkmd_nvrm_dev.c',
  'nvkmd_nvrm_mem.c',
  'nvkmd_nvrm_pdev.c',
  'nvkmd_nvrm_va.c',
  'nvkmd_nvrm_sync.c',
  'nvRmApi.c',
  'nvRmSemSurf.c',
)

# nvkmd_nvrm.h (the backend's public header) transitively pulls in the NVIDIA
# open-gpu-kernel-modules SDK headers (nv.h, nvRmApi.h, nvtypes.h, ctrl/...).
# Export these include dirs through the dependency below so consumers of the
# header (notably nvkmd.c in libnvk) can resolve them.
nvkmd_nvrm_inc = [
  include_directories('.'),       # nvrm/: nvkmd_nvrm.h, nvRmApi.h
  include_directories('../..'),   # the vulkan/ dir: nvkmd/nvkmd.h, nvk_*.h
  include_directories('open-gpu-kernel-modules/src/nvidia/arch/nvalloc/common/inc'),
  include_directories('open-gpu-kernel-modules/src/nvidia/arch/nvalloc/unix/include'),
  include_directories('open-gpu-kernel-modules/src/common/sdk/nvidia/inc'),
  include_directories('open-gpu-kernel-modules/src/nvidia/inc/kernel'),
]

libnvkmd_nvrm = static_library(
  'nvkmd_nvrm',
  nvkmd_nvrm_files,
  include_directories : [
    inc_include,
    inc_src,
    inc_nouveau_winsys,
    nvkmd_nvrm_inc,
  ],
  dependencies : nvk_deps,
  c_args : [no_override_init_args, nvk_flags],
  gnu_symbol_visibility : 'hidden',
)

idep_nvkmd_nvrm = declare_dependency(
  link_with : libnvkmd_nvrm,
  include_directories : nvkmd_nvrm_inc,
)
