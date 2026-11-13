# Fixed backend sources

- FFmpeg **9.0.2**: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
  SHA-256 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`.
- GNU make **4.4.1** (build tool only): https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz
  SHA-256 `dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3`.
- VC-LTL **5.3.1**, YY-Thunks **1.2.2**: fixed URLs/checksums in `cmake/legacy_windows.cmake`.

`cmake/build_matroska.py` enables only the Matroska demuxer and the declared audio
decoders/parsers. No video decoder, encoder, network protocol, program, GPL,
nonfree or version-3-only component is enabled. Libraries are statically linked.
swresample is enabled because FFmpeg's native Opus decoder requires it internally
for SILK; it is not a request to resample decoded music to a fixed device rate.
The plugin contains FFmpeg code under **LGPL-2.1-or-later**, not just the MIT
adapter. FFmpeg notices and the complete corresponding source are retained.

One auditable patch in the build script parses Matroska `FlagEnabled` and exports
it as private stream metadata. Upstream otherwise ignores this flag. Patches are
applied only to the private build copy, never to caller-supplied shared sources.
The complete patched source, patch/build script, GNU make archive, build identity
and adapter are included in the source attachment. Rebuilding it permits
relinking the plugin against a modified LGPL backend.
