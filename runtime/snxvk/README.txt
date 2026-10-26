Native Vulkan renderer (ini renderer=vulkan_native), taken from ScarletNX
(https://github.com/scarletnx/ScarletNX, main branch commit b5f4f14f2bfc, fetched 2026-10-21), itself a fork of
SwitchRecomp. The sources are in src_new/ (src/ in a private checkout is the earlier 2026-10-18 copy, not used).

LICENSE: the code in this folder is GPL-3.0-or-later (LICENSE; MZ Labs' original parts Apache-2.0, LICENSE.apache,
NOTICE). A runtime EXE built with libsnxvk_new.a linked in is therefore a GPL-3.0 work: fine for personal use; anyone
given such an EXE must also be offered this source. A build with SWR_NO_SNXVK=1 leaves it out.

deps/: Vulkan-Headers 1.4.365, volk 1.4.363, VulkanMemoryAllocator 3.4.0, the versions and SHA-256 hashes
ScarletNX's cmake/dependencies.cmake pins (verified).
src_new/tools/: upstream's shaderdec and settingsgen code generators (same project, same licence).

Local changes to upstream (marked "SWR" in the code):
  gpu/gpu.c             plat_prof_mark_gpu (our profiler samples the renderer's GPU thread)
  gpu/vk/hdr.c          HDR output and the FXAA present pass; the pass records through the recording thread's ring
                        (it drained the ring every frame before, which sometimes waited seconds)
  gpu/vk/renderer.c     the HDR/FXAA present pass, DLSS frame hook, "slow present" step timing (logs over 250 ms)
  gpu/vk/render.c, texture.c, device.c, spirv_translate.c: DLSS hooks (dlss.c, private builds only; dlss_stub.c here)
  gpu/vk/swr_settings.c, gpu/maxwell/mme_jit_stub.c: our settings and no LLVM macro JIT (the interpreter runs every macro)

Build (Windows): set GLSLANG=<glslang.exe 16.6.0> and run python\python.exe work-profiler\build_snxvk2.py
(SWR_NO_DLSS=1: runtime\snxvk\libsnxvk_new_nodlss.a, the public build), then the normal --build-runtime links it and
compiles runtime\snxvk_glue.c (the adapter between this renderer and our runtime).

Build (Linux): linux/build_runtime.sh runs work-profiler/build_snxvk_linux.py, which makes
runtime/snxvk/libsnxvk_linux.a with the Xlib Vulkan surface and the checked-in SPIR-V arrays in lin/gen/spv/ (set
GLSLANG to a glslang binary to regenerate them), then links runtime/snxvk_glue_linux.c. The adapter is the POSIX
counterpart of snxvk_glue.c; it has no GetWriteWatch/DXGI and uses ScarletNX's content-checking fallback.

Every global symbol of the ScarletNX objects is renamed with the prefix snxv_ so its GPU front end can sit next to ours.
