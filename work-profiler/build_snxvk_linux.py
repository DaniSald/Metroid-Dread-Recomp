#!/usr/bin/env python3
"""Build the ScarletNX native Vulkan renderer (runtime/snxvk/src_new) for Linux/x86-64.

This is the Linux counterpart of build_snxvk2.py. It produces
runtime/snxvk/libsnxvk_linux.a with every global renamed snxv_<name>, so the
renderer's GPU front end can sit next to the SwitchRecomp runtime. DLSS is
left out (dlss_stub.c), as in the public Windows release.

The SPIR-V arrays in runtime/snxvk/lin/gen/spv are checked in. If GLSLANG points
at a glslang binary they are regenerated first (the Windows build uses 16.6.0;
any 15/16 release works).
"""
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
V = os.path.join(ROOT, 'runtime', 'snxvk')
S = os.path.join(V, 'src_new')
NX = os.path.join(S, 'nx')
OUT = os.path.join(V, 'obj_linux')
GEN = os.path.join(OUT, 'gen')
LIB = os.path.join(V, 'libsnxvk_linux.a')
CC = os.environ.get('CC') or shutil.which('gcc') or 'gcc'
CXX = os.environ.get('CXX') or shutil.which('g++') or 'g++'
AR = os.environ.get('AR') or shutil.which('ar') or 'ar'
LD = os.environ.get('LD') or shutil.which('ld') or 'ld'
NM = os.environ.get('NM') or shutil.which('nm') or 'nm'
OBJCOPY = os.environ.get('OBJCOPY') or shutil.which('objcopy') or 'objcopy'
GLSLANG = os.environ.get('GLSLANG') or shutil.which('glslang') or shutil.which('glslangValidator')

DEPS = {k: os.path.join(V, 'deps', d) for k, d in (
    ('volk', 'volk-vulkan-sdk-1.4.363.0'),
    ('vma', 'VulkanMemoryAllocator-3.4.0'),
    ('vkh', 'Vulkan-Headers-1.4.365'))}
SRC = ('gpu/gpu.c gpu/maxwell/engines.c gpu/maxwell/mme.c gpu/maxwell/mme_hle.c gpu/maxwell/mme_jit_stub.c '
       'gpu/maxwell/compute.c gpu/maxwell/fermi2d.c gpu/maxwell/dma.c gpu/maxwell/write_pages.c gpu/maxwell/maxwell3d.c '
       'gpu/renderer.c gpu/nvn_trace.c gpu/shipped_pass.c '
       'gpu/shader/shader.c gpu/shader/decode.c gpu/shader/corpus.c gpu/shader/shipped.c gpu/shader/spirv_translate.c '
       'gpu/shader/spirv_compute.c gpu/shader/spirv_emit.c '
       'gpu/vk/renderer.c gpu/vk/device.c gpu/vk/cmd.c gpu/vk/record.c gpu/vk/format.c gpu/vk/surface.c gpu/vk/texture.c '
       'gpu/vk/buffer.c gpu/vk/writeback.c gpu/vk/pipeline.c gpu/vk/pipeline_file.c gpu/vk/render.c gpu/vk/release.c '
       'gpu/vk/query.c gpu/vk/compute.c gpu/vk/pending_pages.c gpu/vk/memory.c gpu/vk/renderdoc.c gpu/vk/astc.c '
       'gpu/vk/blocklinear.c gpu/vk/texconv.c gpu/vk/owned.c gpu/vk/dma.c gpu/vk/blit.c gpu/vk/ui.c '
       'gpu/texture/blocklinear.c gpu/texture/depth.c gpu/texture/etc2.c gpu/vk/vic.c gpu/vk/gpuprof.c '
       'gpu/vk/hdr.c gpu/vk/swr_settings.c gpu/vk/dlss_stub.c').split()
REDIRECT = {'g_opt_vsync': 'snxvk_opt_vsync'}
INC = ['-I' + S, '-I' + NX, '-I' + GEN, '-I' + DEPS['volk'], '-I' + os.path.join(DEPS['vma'], 'include'),
       '-I' + os.path.join(DEPS['vkh'], 'include'), '-I' + os.path.join(ROOT, 'runtime')]
CF = ['-std=gnu2x', '-O2', '-fPIC', '-fno-strict-aliasing', '-DVK_NO_PROTOTYPES',
      '-DVK_USE_PLATFORM_XLIB_KHR', '-w'] + INC
SPV = [('gpu/texture/astc.comp', 'astc_spv', 'nx/gpu/texture/astc_spv.h', []),
       ('gpu/texture/blocklinear.comp', 'blocklinear_spv', 'nx/gpu/texture/blocklinear_spv.h', []),
       ('gpu/texture/texconv.comp', 'texconv_spv', 'nx/gpu/texture/texconv_spv.h', []),
       ('gpu/video/vic.comp', 'vic_spv', 'nx/gpu/video/vic_spv.h', []),
       ('gpu/video/vic.comp', 'vic_unaligned_spv', 'nx/gpu/video/vic_spv.h', ['VIC_UNALIGNED']),
       ('gpu/vk/query_predicate.comp', 'query_predicate_spv', 'nx/gpu/vk/query_predicate_spv.h', [])]


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit('build failed: %s\n%s' % (' '.join(cmd[-4:]), (r.stderr or r.stdout)[-6000:]))
    return r.stdout


def gen_tools():
    os.makedirs(GEN, exist_ok=True)
    shaderdec = os.path.join(OUT, 'shaderdec')
    run([CC, '-std=c99', '-O2', '-fPIC', os.path.join(S, 'tools', 'shaderdec', 'shaderdec.c'), '-o', shaderdec])
    run([shaderdec, '--enum', os.path.join(GEN, 'maxwell_opcodes.h'),
         '--decoder', os.path.join(GEN, 'maxwell_decode.inc'),
         os.path.join(NX, 'gpu', 'shader', 'maxwell_opcodes.def')])
    settingsgen = os.path.join(OUT, 'settingsgen')
    run([CC, '-std=c99', '-O2', '-fPIC', os.path.join(S, 'tools', 'settingsgen', 'settingsgen.c'), '-o', settingsgen])
    run([settingsgen, '--out', GEN, os.path.join(NX, 'runtime', 'settings.txt')])


def spirv():
    """Use the checked-in SPIR-V arrays, or regenerate them when GLSLANG is available.
    The arrays are C files, so they can be compiled without a copy of the shader sources."""
    src_dir = os.path.join(OUT, 'spv')
    os.makedirs(src_dir, exist_ok=True)
    if GLSLANG and os.path.isfile(GLSLANG):
        for src, name, header, defs in SPV:
            words = os.path.join(src_dir, name + '.inc')
            run([GLSLANG, '-V', '--target-env', 'vulkan1.1', '-g0', '-x'] + ['-D' + d for d in defs] +
                ['-o', words, os.path.join(NX, src)])
            with open(os.path.join(src_dir, name + '.c'), 'w') as f:
                f.write('/* generated from %s by glslang (build_snxvk_linux.py) */\n#include "%s"\n' % (src, header))
                f.write('const u32 %s[] = {\n#include "%s.inc"\n};\nconst u64 %s_size = sizeof %s;\n' % (name, name, name, name))
        print('SPIR-V regenerated with %s' % GLSLANG)
    else:
        # checked-in generated C arrays: no glslang needed for a normal build
        fallback = os.path.join(V, 'lin', 'gen', 'spv')
        for src, name, header, defs in SPV:
            f = os.path.join(fallback, name + '.c')
            if not os.path.isfile(f):
                sys.exit('missing %s; install glslang or regenerate runtime/snxvk/lin/gen/spv' % f)
            shutil.copy2(f, os.path.join(src_dir, name + '.c'))
        print('SPIR-V from runtime/snxvk/lin/gen/spv (set GLSLANG to regenerate)')
    return [os.path.join(src_dir, name + '.c') for _, name, _, _ in SPV]


def main():
    os.makedirs(OUT, exist_ok=True)
    gen_tools()
    spv_c = spirv()
    jobs = []
    for f in SRC:
        jobs.append((os.path.join(OUT, f.replace('/', '_') + '.o'),
                     [CC] + CF + ['-c', os.path.join(NX, f), '-o', os.path.join(OUT, f.replace('/', '_') + '.o')]))
    for c in spv_c:
        jobs.append((c[:-2] + '.o', [CC] + CF + ['-c', c, '-o', c[:-2] + '.o']))
    jobs.append((os.path.join(OUT, 'settings.o'),
                 [CC] + CF + ['-c', os.path.join(GEN, 'settings.c'), '-o', os.path.join(OUT, 'settings.o')]))
    jobs.append((os.path.join(OUT, 'volk.o'),
                 [CC] + CF + ['-c', os.path.join(DEPS['volk'], 'volk.c'), '-o', os.path.join(OUT, 'volk.o')]))
    with cf.ThreadPoolExecutor(max(1, os.cpu_count() or 4)) as ex:
        list(ex.map(lambda j: run(j[1]), jobs))
    vma = os.path.join(OUT, 'vma.o')
    run([CXX, '-std=c++17', '-O2', '-fPIC', '-w', '-DVK_NO_PROTOTYPES'] + INC +
        ['-c', os.path.join(NX, 'gpu/vk/vma.cpp'), '-o', vma])
    merged = os.path.join(OUT, 'snxvk_c.o')
    run([LD, '-r', '-o', merged] + [o for o, _ in jobs])
    nm = run([NM, '--defined-only', '-g', merged]).split('\n')
    defs = sorted({line.split()[-1] for line in nm if len(line.split()) == 3})
    rename = os.path.join(OUT, 'rename.txt')
    with open(rename, 'w') as f:
        for n in defs:
            f.write('%s snxv_%s\n' % (n, n))
        for a, b in REDIRECT.items():
            f.write('%s %s\n' % (a, b))
    renamed = os.path.join(OUT, 'snxvk_renamed.o')
    run([OBJCOPY, '--redefine-syms=' + rename, merged, renamed])
    if os.path.exists(LIB):
        os.remove(LIB)
    vma_r = os.path.join(OUT, 'vma_renamed.o')
    run([OBJCOPY, '--redefine-syms=' + rename, vma, vma_r])
    run([AR, 'rcs', LIB, renamed, vma_r])
    und = run([NM, '-u', renamed]).split()
    ext = sorted(x for x in set(und) if x != 'U' and not x.startswith('__imp_') and not x.startswith('___'))
    with open(os.path.join(OUT, 'externals.txt'), 'w') as f:
        f.write('\n'.join(ext) + '\n')
    print('%s: %d symbols renamed; %d external references (externals.txt)' % (LIB, len(defs), len(ext)))


if __name__ == '__main__':
    main()
