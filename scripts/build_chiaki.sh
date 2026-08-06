#!/usr/bin/env bash
# ==========================================================
# chiaki-rocknix — reproducible build of the ROCKNIX SDL2 frontend
# ==========================================================
# Builds rocknix/ as an aarch64/glibc binary that runs natively on ROCKNIX.
# Runs on ANY arm64 Docker host (etk-cloud natively, the Air via colima).
#
# This script NEVER touches the rig. The device-side NEEDED check is the
# staging host's job (etk/tools/rocknix-bin/build_chiaki.sh), because the rig
# is only ever reached from the LAN-local Mac.
#
# WHY A CONTAINER: ROCKNIX is read-only and ships no toolchain. We build in an
# Ubuntu 24.04 *arm64* container (glibc 2.39). glibc is backward-compatible, so
# a binary linked against 2.39 runs on the rig's 2.41. Noble is the one image
# whose ffmpeg (6.1 -> libavcodec.so.60) matches the rig's ffmpeg 6.0.1 SONAME
# exactly; SDL2 2.30 and OpenSSL 3 line up the same way.
#
# libopus is NOT on the rig's loader path (/usr/lib/compat is box64-only), so
# opus is linked STATICALLY from noble's libopus.a.
#
# TOOLCHAIN PARITY IS NOT AUTOMATIC. `ubuntu:24.04` is a moving tag: on
# 2026-08-05 the Air's noble was 786a8b55 while etk-cloud's was 561618e2 -- two
# different images behind one tag. The digest below is the validated one; the
# NEEDED gate is what actually catches a base-image drift that matters (an
# ffmpeg 7 noble would silently produce libavcodec.so.61 and not load on the
# rig). Override with BASE_IMAGE=... to test a new base deliberately.
#
# Outputs (into $OUT_DIR, default ./out):
#   chiaki            stripped aarch64 binary
#   chiaki.ldd        NEEDED manifest -- the contract with the device
#   chiaki.commit     fork SHA (-dirty if the tree is uncommitted)
#   chiaki.buildinfo  provenance: base digest, toolchain + library versions
# ==========================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OUT_DIR="${OUT_DIR:-$REPO/out}"

# Validated noble (etk-cloud, 2026-08-05). Pinned by digest on purpose.
BASE_IMAGE="${BASE_IMAGE:-ubuntu@sha256:561618e2c15bf2397621dd04f96926663a3b5616c189cf7e38db7e82f5c538ea}"

# The device contract. Any deviation -- an opus leak, an ffmpeg SONAME bump --
# means the binary will not load on ROCKNIX. Verified against the rig 2026-07-29.
EXPECTED_NEEDED="ld-linux-aarch64.so.1
libSDL2-2.0.so.0
libavcodec.so.60
libavutil.so.58
libc.so.6
libcrypto.so.3
libm.so.6"

if [ ! -f "$REPO/rocknix/CMakeLists.txt" ]; then
    echo "ERROR: no rocknix frontend at $REPO -- wrong repo or wrong branch?" >&2
    exit 1
fi
if [ ! -f "$REPO/third-party/nanopb/generator/nanopb_generator.py" ]; then
    echo "ERROR: submodules missing -- run: git submodule update --init --recursive" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

# Provenance stamp (host-side git; the container image has no git).
COMMIT="$(git -C "$REPO" rev-parse HEAD)"
if ! git -C "$REPO" diff --quiet HEAD 2>/dev/null; then
    COMMIT="$COMMIT-dirty"
fi
echo "$COMMIT" > "$OUT_DIR/chiaki.commit"

echo "building chiaki @ $COMMIT"
echo "base image: $BASE_IMAGE"

docker run --rm --platform linux/arm64 \
    -e "HOST_UID=$(id -u)" -e "HOST_GID=$(id -g)" \
    -v "$REPO:/src:ro" -v "$OUT_DIR:/out" "$BASE_IMAGE" bash -c '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq build-essential cmake ninja-build pkg-config \
    protobuf-compiler python3-protobuf python3-setuptools \
    libssl-dev libopus-dev libavcodec-dev libavutil-dev libsdl2-dev >/dev/null

  OPUS_A=/usr/lib/aarch64-linux-gnu/libopus.a
  if [ ! -f "$OPUS_A" ]; then
    echo "ERROR: static libopus.a missing from libopus-dev" >&2
    exit 1
  fi

  # Writable copy: the nanopb generator writes nanopb_pb2.py into its own
  # source dir, which the read-only mount (deliberately) forbids.
  mkdir -p /root/srcw
  tar -C /src --exclude=.git --exclude=out -cf - . | tar -C /root/srcw -xf -

  cmake -S /root/srcw -B /root/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCHIAKI_ENABLE_GUI=OFF \
    -DCHIAKI_ENABLE_TESTS=OFF \
    -DCHIAKI_ENABLE_CLI=ON \
    -DCHIAKI_ENABLE_ROCKNIX=ON \
    -DCHIAKI_ENABLE_SETSU=OFF \
    -DCHIAKI_ENABLE_PI_DECODER=OFF \
    -DCHIAKI_ENABLE_FFMPEG_DECODER=ON \
    -DOpus_INCLUDE_DIRS=/usr/include \
    -DOpus_LIBRARIES="$OPUS_A;/usr/lib/aarch64-linux-gnu/libm.so"
  cmake --build /root/build

  BIN=/root/build/rocknix/chiaki
  strip "$BIN"

  readelf -d "$BIN" | awk "/NEEDED/ {gsub(/[\[\]]/,\"\",\$5); print \$5}" > /out/chiaki.ldd

  # Native container: the binary itself must run (catches ABI screwups early).
  "$BIN" --help >/dev/null

  install -m 0755 "$BIN" /out/chiaki

  # Provenance: what this artifact was actually built against.
  {
    echo "built_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "base_image_id=$(cat /etc/os-release | grep ^VERSION= | cut -d= -f2- | tr -d \")"
    echo "gcc=$(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion)"
    for p in libavcodec-dev libavutil-dev libsdl2-dev libssl-dev libopus-dev; do
      echo "$p=$(dpkg-query -W -f=\${Version} $p)"
    done
  } > /out/chiaki.buildinfo

  # The container is root; hand the artifacts back to the invoking user so the
  # host-side gates can append. (No-op/ignored on colima virtiofs mounts.)
  chown "$HOST_UID:$HOST_GID" /out/chiaki /out/chiaki.ldd /out/chiaki.buildinfo 2>/dev/null || true
'

# ---- gates (host side, so a container that lies still gets caught) ----
ACTUAL_NEEDED="$(sort "$OUT_DIR/chiaki.ldd")"
if [ "$ACTUAL_NEEDED" != "$(echo "$EXPECTED_NEEDED" | sort)" ]; then
    echo "ERROR: NEEDED manifest does not match the device contract." >&2
    echo "--- expected ---" >&2; echo "$EXPECTED_NEEDED" | sort >&2
    echo "--- actual ---"   >&2; echo "$ACTUAL_NEEDED" >&2
    echo "A base-image drift (ffmpeg SONAME bump, failed static opus) will look like this." >&2
    echo "Do NOT deploy. Re-pin BASE_IMAGE or fix the link." >&2
    exit 1
fi

{
    echo "commit=$COMMIT"
    echo "base_image=$BASE_IMAGE"
    echo "sha256=$( (sha256sum "$OUT_DIR/chiaki" 2>/dev/null || shasum -a 256 "$OUT_DIR/chiaki") | awk '{print $1}')"
    echo "bytes=$(wc -c < "$OUT_DIR/chiaki" | tr -d ' ')"
} >> "$OUT_DIR/chiaki.buildinfo"

echo "--- NEEDED (matches device contract) ---"
cat "$OUT_DIR/chiaki.ldd"
echo "--- buildinfo ---"
cat "$OUT_DIR/chiaki.buildinfo"
echo "OK -> $OUT_DIR/chiaki"
