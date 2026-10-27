# Software video implementation

- FFmpeg 7.1.2: https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz
  SHA256: 089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304.
  Built from source by `app/cmake/VideoDecoder.cmake`, LGPL-2.1-or-later,
  without GPL/nonfree/external codec dependencies. Local patch
  `app/patches/ffmpeg/nvdec-decode-order.patch` returns H264 pictures in decode
  order when LOW_DELAY is enabled. The guest owns presentation order. Decoder
  references remain managed by libavcodec. The patched decoder is private to
  this application. Source, patch and build recipe must accompany distribution;
  LGPL static-link distributions also need a way to relink the library (retain
  the application object files/build outputs). See FFMPEG-LICENSE.txt.
- Ryujinx, MIT, copyright Ryujinx Team and Contributors. H264 SPS/PPS
  reconstruction, Tegra picture/VIC register layouts and matrix semantics are
  adapted from commit 5189a807c45722e3ed9173dd90ed87a4606d1a89:
  https://git.axenov.dev/Museum/ryujinx/src/commit/5189a807c45722e3ed9173dd90ed87a4606d1a89
  VP8 header reconstruction checked against commit
  a4fc9f8050405c8589793d93caa8f259d1618708. See RYUJINX-LICENSE.txt.
- NVIDIA register/picture definitions were consulted:
  https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/video/nvdec_drv.h
  No NVIDIA driver binaries are used.

This implements CPU decoding, not direct access to NVIDIA hardware on Android.
NVDEC: H264 progressive 8-bit 4:2:0 POC 0/2 (parameter-set ID 0) and VP8,
stateful sequences in decode order. SPS/PPS, VP8 uncompressed header, Tegra NV12
surface writeback, optional decoder status, separate state per open channel.
Not implemented: VP9, HEVC, VC1, MPEG, interlaced H264, POC 1, arbitrary guest
reference-buffer replacement/import, encrypted input, display postprocessing.
NVDEC metadata references/motion-vector surfaces are not exported to guests.

VIC: one opaque slot or background fill; NV12/NV21 and RGBA/BGRA/RGBX/BGRX;
linear and Tegra block-linear; source/destination rectangles, bilinear scaling,
flip X/Y, signed color/gamut/output matrices, constant alpha and clamp; output
NV12/NV21 or packed 32-bit RGB. Unsupported blending, deinterlacing, advanced
filters, compression, transpose, partial output target rectangles and nonzero
alpha-fill modes fail explicitly. This is a subset of VIC, not full hardware
emulation. Bilinear sampling does not emulate programmable filter coefficients.

Submit is synchronous (including memory writes); it copies commands, applies
relocations to the copy, pins relocation targets, validates packet boundaries,
executes commands and only returns fences after completion. It does not invent
syncpoint increments. A failed execution invalidates the channel; restart the
emulation session because already executed commands/counters cannot be undone.
Host1x waits default to 5 seconds; SetSubmitTimeout sets the per-wait timeout,
not a total CPU decoder deadline. CPU decoding/scaling cost on phones has not
been benchmarked. The application never creates an intermediate MP4 file.
