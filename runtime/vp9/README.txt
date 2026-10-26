libvpx 1.14.1 (BSD-3-Clause, see LICENSE-libvpx.txt), VP9 decoder only, built for Windows x64
with mingw-w64 GCC 13 (--target=x86_64-win64-gcc, nasm optimisations on, single-threaded: --disable-multithread).
Used by runtime/vp9video.inc to play the game's .webm videos.

Linux x86-64: libvpx 1.16.0 (same BSD-3-Clause licence, from Ubuntu's libvpx-dev 1.16.0-3),
prebuilt in lin/libvpx.a with the matching headers in include/vpx/. The Linux runtime links it
statically when SWR_HAVE_VP9 is enabled (see switchrecomp.py build_runtime). Metroid Dread's
.webm videos are VP9, so without this library the title screen and cutscenes stay black.
To rebuild the Linux archive from upstream libvpx 1.16.0 with the same decoder options:
  ./configure --target=x86_64-linux-gcc --disable-vp8 --disable-vp9-encoder \
              --disable-multithread --disable-shared --enable-static
  make -j"$(nproc)"
  cp libvpx.a runtime/vp9/lin/
  cp vpx/*.h runtime/vp9/include/vpx/
