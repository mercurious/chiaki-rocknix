# Building chiaki-rocknix

The target is ROCKNIX on aarch64 (glibc). No cross-toolchain and no ROCKNIX
SDK are required: build in a **native arm64 container** against an older
glibc and link only libraries the device already ships.

## Why this works

| dependency | build container (ubuntu:24.04) | ROCKNIX (verified 2026-07) |
|---|---|---|
| glibc | 2.39 | 2.41 (backward compatible) |
| libavcodec | 6.1 -> `libavcodec.so.60` | 6.0.1 -> `libavcodec.so.60` |
| libSDL2 | 2.30 -> `libSDL2-2.0.so.0` | 2.32 -> `libSDL2-2.0.so.0` |
| OpenSSL | 3.x -> `libcrypto.so.3` | 3.x -> `libcrypto.so.3` |
| opus | **statically linked** | not on the loader path |

## Recipe

On any arm64 host with Docker (Apple Silicon via colima runs it natively;
x86 hosts work through qemu, slower):

```sh
git clone -b rocknix --recurse-submodules https://github.com/mercurious/chiaki-rocknix.git
cd chiaki-rocknix

docker run --rm --platform linux/arm64 -v "$PWD:/src:ro" -v "$PWD/out:/out" ubuntu:24.04 bash -c '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq build-essential cmake ninja-build pkg-config \
    protobuf-compiler python3-protobuf python3-setuptools \
    libssl-dev libopus-dev libavcodec-dev libavutil-dev libsdl2-dev >/dev/null
  mkdir -p /root/srcw && tar -C /src --exclude=.git -cf - . | tar -C /root/srcw -xf -
  cmake -S /root/srcw -B /root/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCHIAKI_ENABLE_GUI=OFF -DCHIAKI_ENABLE_TESTS=OFF \
    -DCHIAKI_ENABLE_CLI=ON -DCHIAKI_ENABLE_ROCKNIX=ON \
    -DCHIAKI_ENABLE_SETSU=OFF -DCHIAKI_ENABLE_PI_DECODER=OFF \
    -DCHIAKI_ENABLE_FFMPEG_DECODER=ON \
    -DOpus_INCLUDE_DIRS=/usr/include \
    -DOpus_LIBRARIES="/usr/lib/aarch64-linux-gnu/libopus.a;/usr/lib/aarch64-linux-gnu/libm.so"
  cmake --build /root/build
  strip /root/build/rocknix/chiaki
  install -m 0755 /root/build/rocknix/chiaki /out/chiaki
  readelf -d /out/chiaki | awk "/NEEDED/ {gsub(/[\[\]]/,\"\",\$5); print \$5}"
'
```

Output: `out/chiaki` (~800 KB stripped). The final `readelf` prints the NEEDED
list — every SONAME it shows must exist in `/usr/lib` on the device
(`libSDL2-2.0.so.0 libavcodec.so.60 libavutil.so.58 libcrypto.so.3` + glibc).
If opus appears in that list the static link failed; do not deploy.

Notes:
- The writable source copy inside the container exists because the nanopb
  generator writes into its own source dir.
- The read-only mount plus out-of-tree build keeps your checkout pristine.
- `libm.so` rides in `Opus_LIBRARIES` so it links after the static opus
  archive (link order matters for `exp()` etc.).
