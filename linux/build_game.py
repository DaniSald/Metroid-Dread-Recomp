#!/usr/bin/env python3
"""Build Metroid Dread for the native Linux runtime.

This is the Linux counterpart of dread/dread_setup.py: it checks the player's own
ExeFS (v2.1.0), installs the RomFS, translates the game to C, applies the verified
Dread patches and compiles <name>_code.so with the system GCC.  The runtime itself
is built by linux/build_runtime.sh (or `build_all.sh`, which runs both).

Usage:
  python3 linux/build_game.py /path/to/dump
  python3 linux/build_game.py /path/to/dump --update /path/to/update --jobs 4
  python3 linux/build_game.py /path/to/dump --out linux/dist --romfs-mode symlink

The dump must contain exefs/ and romfs/ (or be the ExeFS/RomFS folders' parent).
No Nintendo files are included in the repository; this only compiles the user's
own copy.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, 'dread'))

import switchrecomp, nxloader, codegen, metroid_perf
import dread_setup

dread_setup.PATCHES = os.path.join(ROOT, 'dread', 'dread_v210_patches.json')
dread_setup.MANIFEST = os.path.join(ROOT, 'dread', 'romfs_v210.manifest')

DEFAULT_NAME = 'Metroid Dread'
RUNTIME = os.path.join(ROOT, 'bin', 'runtime_linux_x11')
COMPAT = os.path.join(HERE, 'lx_compat.h')


def say(msg):
    print(msg, flush=True)


def fail(msg):
    raise SystemExit('error: ' + msg)


def remove_path(path):
    if os.path.islink(path) or os.path.isfile(path):
        os.remove(path)
    elif os.path.isdir(path):
        shutil.rmtree(path)


def install_romfs(src, update_src, dst, mode):
    """Put the RomFS next to exefs. copy/hardlink verify against the 2.1.0 manifest;
    symlink is a fast development mode that leaves the files where they are."""
    if mode == 'none':
        if not os.path.isdir(dst):
            fail('no RomFS in %s and --romfs-mode none was given' % dst)
        say('RomFS: using the existing %s' % dst)
        return
    if mode == 'symlink':
        if update_src and os.path.abspath(update_src) != os.path.abspath(src):
            fail('--romfs-mode symlink cannot layer a separate update RomFS; use copy or hardlink')
        if os.path.exists(dst) or os.path.islink(dst):
            remove_path(dst)
        os.symlink(os.path.abspath(src), dst)
        say('RomFS: linked %s -> %s (development mode, not a self-contained package)' % (dst, src))
        return
    # switching from a development symlink: remove it so the copy never writes into the source
    if os.path.islink(dst):
        os.remove(dst)
    # copy/hardlink: use the setup's manifest check and layering
    dread_setup.assemble_romfs(src, update_src, dst, 0, 100)
    if mode == 'hardlink':
        # assemble_romfs already hard-links when the source and destination are on
        # the same filesystem and copies otherwise; nothing more to do here.
        pass


def main():
    ap = argparse.ArgumentParser(description='Compile a Metroid Dread 2.1.0 dump for the Linux runtime.')
    ap.add_argument('game', help='folder with exefs/ and romfs/ (base game, or base + update below it)')
    ap.add_argument('--update', default='', help='separate 2.1.0 update folder (if the base dump is not merged)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'games'), help='output parent folder (default: games/)')
    ap.add_argument('--name', default=DEFAULT_NAME, help='game name / output prefix (default: Metroid Dread)')
    ap.add_argument('--cc', default='gcc', help='C compiler (default: gcc)')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) // 2),
                    help='parallel compiler jobs (default: half the CPU count)')
    ap.add_argument('--chunk', type=int, default=30000, help='instructions per generated C file')
    ap.add_argument('--romfs-mode', choices=['auto', 'copy', 'hardlink', 'symlink', 'none'], default='auto',
                    help='how to install the RomFS (default: hardlink when possible, else copy)')
    ap.add_argument('--force', action='store_true', help='regenerate the C even when the cached generation is current')
    ap.add_argument('--skip-runtime', action='store_true', help='do not copy/build the runtime executable')
    args = ap.parse_args()

    cc = shutil.which(args.cc)
    if not cc:
        fail('C compiler not found: %s (install build-essential)' % args.cc)
    game = os.path.abspath(args.game)
    if not os.path.isdir(game):
        fail('game folder does not exist: %s' % game)
    update = os.path.abspath(args.update) if args.update else ''
    if update and not os.path.isdir(update):
        fail('update folder does not exist: %s' % update)

    say('Metroid Dread Linux build - use only your own copy of the game.')
    say('Game folder: %s' % game)
    if update:
        say('Update folder: %s' % update)

    # ---------------------------------------------------------------- ExeFS / RomFS
    try:
        exefs = dread_setup.pick_exefs(game, update)
        base_romfs = dread_setup.romfs_in(game)
        upd_romfs = dread_setup.romfs_in(update) if update else None
        if upd_romfs and base_romfs and os.path.samefile(upd_romfs, base_romfs):
            upd_romfs = None
        if not base_romfs and not upd_romfs:
            fail('no RomFS found (expected a romfs/ folder with system/, packs/, ...)')
    except dread_setup.Fail as e:
        fail(str(e))
    say('ExeFS: %s' % exefs)
    say('RomFS: base %s, update %s' % (base_romfs or '-', upd_romfs or '-'))

    title = ''.join(ch for ch in args.name if ch.isalnum() or ch in '-_ ').strip() or DEFAULT_NAME
    outdir = os.path.abspath(args.out)
    pkg = os.path.join(outdir, title)
    data = os.path.join(pkg, title + '_data')
    build = os.path.join(pkg, title + '_build')
    exe = os.path.join(pkg, title)
    code = os.path.join(pkg, title + '_code.so')
    os.makedirs(pkg, exist_ok=True)
    os.makedirs(data, exist_ok=True)
    os.makedirs(build, exist_ok=True)

    ex_dst = os.path.join(data, 'exefs')
    os.makedirs(ex_dst, exist_ok=True)
    if os.path.samefile(exefs, ex_dst):
        say('ExeFS: installed copy is up to date')
    else:
        dread_setup.copy_exefs(exefs, ex_dst)

    rom_mode = args.romfs_mode
    if rom_mode == 'auto':
        try:
            same = os.stat(base_romfs or upd_romfs).st_dev == os.stat(data).st_dev
        except OSError:
            same = False
        rom_mode = 'hardlink' if same else 'copy'
        say('RomFS: auto mode -> %s' % rom_mode)
    if base_romfs:
        install_romfs(base_romfs, upd_romfs, os.path.join(data, 'romfs'), rom_mode)
    elif upd_romfs:
        fail('only an update RomFS was found; also provide the base game RomFS')

    # ---------------------------------------------------------------- translate
    mods, npdm, _ = nxloader.load_program(data)
    for m in mods:
        say('  %-8s %010X  code %.1f MB' % (m.name, m.base, m.text[1] / 1048576.0))
    info = nxloader.parse_npdm(npdm)
    if not info.get('title_id'):
        fail('main.npdm is missing or invalid')

    pat = open(dread_setup.PATCHES, 'rb').read()
    gen_key = hashlib.sha1(open(os.path.join(ex_dst, 'main'), 'rb').read()[:0x100] + pat
                           + open(os.path.join(ROOT, 'codegen.py'), 'rb').read()
                           + open(os.path.join(ROOT, 'metroid_perf.py'), 'rb').read()).hexdigest()
    marker = os.path.join(build, '.generated')
    files = []
    regen = args.force or not (os.path.isfile(marker) and open(marker).read().strip() == gen_key)
    if regen:
        say('Analyzing the code (7 million instructions; this takes a few minutes)...')
        prog = codegen.Program(mods, [])
        prog.analyze(say)
        say('Translating ARM64 to C...')
        files = switchrecomp.gen_module(prog, mods, npdm, build, title, args.chunk, say)
        say('Applying the verified Metroid Dread 2.1.0 patches...')
        try:
            dread_setup.apply_patches(build)
        except dread_setup.Fail as e:
            fail(str(e))
        with open(marker, 'w') as f:
            f.write(gen_key)
    else:
        say('Generated C is current: reusing it (pass --force to regenerate).')
        files = sorted(os.path.join(build, f) for f in os.listdir(build)
                       if (f.startswith('code_') and f.endswith('.c')) or f == 'module.c')
        files.sort(key=lambda p: (os.path.basename(p) != 'module.c', p))
    if not files:
        fail('no generated C files in %s' % build)

    # ---------------------------------------------------------------- compile / link
    compat_hash = hashlib.sha1(open(COMPAT, 'rb').read()).hexdigest()[:10]
    extra = ['-include', COMPAT, '-DSWR_LX_COMPAT=' + compat_hash]
    say('Compiling %d files with %s (-O2, %d jobs)...' % (len(files), os.path.basename(cc), args.jobs))
    t0 = time.time()
    try:
        objs = switchrecomp.compile_all(files, cc, '2', build, False, args.jobs, say, extra)
        say('Linking %s ...' % os.path.basename(code))
        switchrecomp.link_module(objs, cc, code, False, say)
    except SystemExit as e:
        fail(str(e))
    if not os.path.isfile(code) or os.path.getsize(code) < 20 * 2**20:
        fail('linking produced no usable game code: %s' % code)
    say('Game code: %s (%.1f MB, %.0f s)' % (code, os.path.getsize(code) / 2**20, time.time() - t0))

    # ---------------------------------------------------------------- runtime + ini
    if not args.skip_runtime:
        if not os.path.isfile(RUNTIME):
            fail('%s is missing: run linux/build_runtime.sh first (or use linux/build_all.sh)' % RUNTIME)
        shutil.copy2(RUNTIME, exe)
        os.chmod(exe, 0o755)
        say('Runtime: %s' % exe)
    ini = os.path.join(pkg, title + '.ini')
    if not os.path.exists(ini):
        with open(ini, 'w', encoding='utf-8') as f:
            f.write('; %s - SwitchRecomp Linux settings\n'
                    '; The in-game menu (F1 or the controller Home button) changes these too.\n'
                    '[Display]\nfullscreen=0\ndocked=1\nlanguage=1\n'
                    '[Graphics]\nrenderer=opengl\nresolution=1.0\nfps_limit=60\n'
                    '[Input]\ncontroller_api=0\n' % title)
    say('')
    say('Done. Run:')
    say('  cd "%s" && ./%s' % (pkg, title))
    say('Game data: %s' % data)


if __name__ == '__main__':
    main()
