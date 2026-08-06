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
./scripts/build_chiaki.sh
```

Output in `out/`:

| file | what it is |
|---|---|
| `chiaki` | stripped aarch64 binary (~800 KB) |
| `chiaki.ldd` | NEEDED manifest — the contract with the device |
| `chiaki.commit` | fork SHA (`-dirty` if the tree is uncommitted) |
| `chiaki.buildinfo` | base image digest, gcc + library versions, sha256, size |

The script is the single source of truth for the recipe: the same file builds
on a cloud aarch64 box and on Apple Silicon via colima, and both produce a
**byte-identical** binary (verified 2026-08-05, sha256 `6ab192e7…`).

## Gates

The build fails closed rather than producing an undeployable binary:

- **NEEDED allowlist.** The manifest must be exactly `libSDL2-2.0.so.0
  libavcodec.so.60 libavutil.so.58 libcrypto.so.3 libm.so.6 libc.so.6
  ld-linux-aarch64.so.1`. This catches both an opus leak (static link failed)
  and an ffmpeg SONAME bump — a noble that moved to ffmpeg 7 would silently
  emit `libavcodec.so.61`, which cannot load on the device.
- **Smoke test.** `chiaki --help` runs inside the native container, so an ABI
  mistake surfaces at build time instead of on the handheld.

## Pinning

`ubuntu:24.04` is a moving tag, so `BASE_IMAGE` is pinned **by digest**. This
is not theoretical: on 2026-08-05 two build hosts held different images behind
that one tag. Override deliberately to qualify a new base:

```sh
BASE_IMAGE=ubuntu:24.04 ./scripts/build_chiaki.sh   # then check the gates pass
```

If the NEEDED gate fails after a base change, the new base is not usable —
re-pin to the digest recorded in `chiaki.buildinfo` of a known-good build.

## Notes

- The writable source copy inside the container exists because the nanopb
  generator writes into its own source dir.
- The read-only mount plus out-of-tree build keeps your checkout pristine.
- `libm.so` rides in `Opus_LIBRARIES` so it links after the static opus
  archive (link order matters for `exp()` etc.).
- The script never contacts the device. Verifying the NEEDED sonames against a
  real ROCKNIX install is the deploying host's job.
