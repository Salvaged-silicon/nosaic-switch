#!/usr/bin/env bash
#
# Build a NOSaic cross-toolchain with crosstool-NG.
#
#   bootstrap/build.sh build <arch>    build the toolchain into toolchain/<arch>
#   bootstrap/build.sh seed  <arch>    regenerate bootstrap/configs/<arch>.defconfig
#   bootstrap/build.sh test  <arch>    compile and run a hello-world with it
#
# Runs inside the builder container; see the Makefile. crosstool-NG itself is
# fetched and hash-verified here rather than baked into the image, so that the
# toolchain's provenance is in the repository and not in a container layer.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# shellcheck source=bootstrap/versions.env
source bootstrap/versions.env

DL="$ROOT/dl"
CACHE="$ROOT/.cache"
CTNG_SRC="$CACHE/crosstool-ng-$CTNG_VERSION"
CTNG="$CTNG_SRC/ct-ng"
# Deliberately not $(nproc). GCC's memory use scales with parallelism, and an
# unconstrained toolchain build will exhaust a modest machine. The Makefile
# passes a considered value; this is the floor for a direct invocation.
JOBS="${JOBS:-2}"

log() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

arch_field() {
    # Read one field out of arch/<arch>/arch.yml without a YAML parser: this
    # script must work before anything Go is built.
    local arch="$1" field="$2"
    sed -n "s/^${field}:[[:space:]]*//p" "arch/$arch/arch.yml" | head -1 | tr -d '"'
}

require_arch() {
    [ -f "arch/$1/arch.yml" ] || die "unknown arch '$1' (no arch/$1/arch.yml)"
}

# ---------------------------------------------------------------- crosstool-NG

fetch_ctng() {
    mkdir -p "$DL" "$CACHE"
    local tarball="$DL/crosstool-ng-$CTNG_VERSION.tar.xz"

    if [ ! -f "$tarball" ]; then
        log "fetching crosstool-NG $CTNG_VERSION"
        got=no
        for url in "$CTNG_URL" ${CTNG_MIRROR:-}; do
            # --retry covers a transient reset; -C - resumes rather than
            # starting the transfer again from nothing.
            if curl -fsSL --retry 3 --retry-delay 2 -C - -o "$tarball.part" "$url"; then
                got=yes
                break
            fi
            printf '    %s did not answer; trying the next source\n' "$url"
        done
        [ "$got" = yes ] || die "could not fetch crosstool-NG from any source"
        mv "$tarball.part" "$tarball"
    fi

    # Verified on every run, not just on download: a cached file can rot.
    echo "$CTNG_SHA256  $tarball" | sha256sum -c - >/dev/null \
        || die "crosstool-NG checksum mismatch — refusing to build from it"

    if [ ! -x "$CTNG" ]; then
        log "building crosstool-NG $CTNG_VERSION"
        rm -rf "$CTNG_SRC"
        tar --no-same-owner -C "$CACHE" -xJf "$tarball"
        ( cd "$CTNG_SRC" && ./configure --enable-local >/dev/null && make -j"$JOBS" >/dev/null )
    fi
    "$CTNG" version | sed -n 1p
}

# ------------------------------------------------------------------ defconfig

# Overrides applied on top of the upstream sample. Everything here is a
# deliberate NOSaic decision; everything not here is crosstool-NG's default,
# which is the tested combination.
ctng_overrides() {
    local arch="$1"
    cat <<EOF
CT_TARGET_VENDOR="nosaic"
CT_PREFIX_DIR="\${CT_TOP_DIR}/../../toolchain/$arch"

# The build runs in a single-purpose container. Rootless Docker maps container
# root to the invoking user, so dropping privileges here would write files the
# host cannot read. crosstool-NG supports this case explicitly.
CT_EXPERIMENTAL=y
CT_ALLOW_BUILD_AS_ROOT=y
CT_ALLOW_BUILD_AS_ROOT_SURE=y

# Keep downloaded component tarballs in the repo's dl/ so a rebuild is offline
# and so the exact sources are inspectable.
CT_LOCAL_TARBALLS_DIR="\${CT_TOP_DIR}/../../dl"
CT_SAVE_TARBALLS=y

# glibc's minimum supported kernel. Set explicitly, because the per-sample
# default is NOT uniform: the x86_64 sample sets NONE (floor 3.2.0) while the
# aarch64 and powerpc samples inherit AS_HEADERS, which pinned them to 6.16.0.
# A binary built that way aborts at startup with "kernel too old" on anything
# older -- including the AS5610, which runs 5.10/6.1. NOSaic exists for
# end-of-service-life hardware, much of which is stuck on a vendor kernel that
# will never be updated, so the floor stays at glibc's own minimum.
CT_GLIBC_KERNEL_VERSION_NONE=y
# CT_GLIBC_KERNEL_VERSION_AS_HEADERS is not set
# CT_GLIBC_KERNEL_VERSION_CHOSEN is not set
EOF
}

cmd_seed() {
    local arch="$1"; require_arch "$arch"
    local sample; sample="$(arch_field "$arch" ctng_sample)"
    [ -n "$sample" ] || die "arch/$arch/arch.yml has no ctng_sample"

    fetch_ctng
    local work="$CACHE/seed-$arch"
    rm -rf "$work"; mkdir -p "$work"

    log "seeding $arch from crosstool-NG sample '$sample'"
    ( cd "$work" && "$CTNG" "$sample" >/dev/null )
    ctng_overrides "$arch" >> "$work/.config"
    ( cd "$work" && "$CTNG" olddefconfig >/dev/null && "$CTNG" savedefconfig >/dev/null )

    mkdir -p bootstrap/configs
    {
        echo "# crosstool-NG defconfig for $arch — generated by bootstrap/build.sh seed"
        echo "# Seeded from upstream sample: $sample"
        echo "# crosstool-NG: $CTNG_VERSION"
        echo "#"
        echo "# Regenerate with: make toolchain-seed ARCH=$arch"
        echo "# Do not hand-edit; put deliberate choices in ctng_overrides()."
        cat "$work/defconfig"
    } > "bootstrap/configs/$arch.defconfig"

    echo "wrote bootstrap/configs/$arch.defconfig ($(wc -l < "bootstrap/configs/$arch.defconfig") lines)"
}

# ---------------------------------------------------------------------- build

cmd_build() {
    local arch="$1"; require_arch "$arch"
    local defconfig="bootstrap/configs/$arch.defconfig"
    [ -f "$defconfig" ] || die "$defconfig missing — run: make toolchain-seed ARCH=$arch"

    fetch_ctng
    local work="$CACHE/build-$arch"
    mkdir -p "$work" "$DL"

    log "building the $arch toolchain (this takes a while)"
    cp "$defconfig" "$work/defconfig"
    ( cd "$work" && DEFCONFIG="defconfig" "$CTNG" defconfig >/dev/null )
    ( cd "$work" && CT_JOBS="$JOBS" "$CTNG" build )

    local triple; triple="$(arch_field "$arch" triple)"
    # crosstool-NG bakes the configured prefix into the compiler as an absolute
    # sysroot path, so a toolchain built to the wrong prefix compiles nothing
    # and says only "stdio.h: No such file or directory". Fail here, where the
    # cause is obvious, rather than an hour later in a recipe.
    local sysroot
    sysroot="$("toolchain/$arch/bin/$triple-gcc" -print-sysroot)"
    if [ ! -d "$sysroot" ]; then
        die "toolchain built with a bad prefix: it looks for its sysroot at
       $sysroot
     which does not exist. Check CT_PREFIX_DIR in bootstrap/configs/$arch.defconfig
     (CT_TOP_DIR is the build directory, not the repository root)."
    fi

    printf 'arch=%s\ntriple=%s\ncrosstool_ng=%s\nsysroot=%s\n' \
        "$arch" "$triple" "$CTNG_VERSION" "$sysroot" > "toolchain/$arch.stamp"
    log "toolchain/$arch ready (sysroot: $sysroot)"
}

# ----------------------------------------------------------------------- test

# The gate: a toolchain that builds but produces binaries that do not run has
# not been proven by anything.
cmd_test() {
    local arch="$1"; require_arch "$arch"
    local triple qemu cc out
    triple="$(arch_field "$arch" triple)"
    qemu="$(arch_field "$arch" qemu)"
    cc="toolchain/$arch/bin/$triple-gcc"

    [ -x "$cc" ] || die "$cc not found — build the toolchain first"

    out="$CACHE/test-$arch"
    mkdir -p "$out"
    log "testing $arch ($triple)"

    "$cc" -static -O2 -o "$out/hello" bootstrap/test/hello.c
    file "$out/hello" | sed 's/^/  /'

    local result
    if [ -n "$qemu" ]; then
        command -v "$qemu" >/dev/null || die "$qemu not installed; cannot run $arch binaries"
        result="$("$qemu" "$out/hello")"
    else
        result="$("$out/hello")"
    fi
    echo "  ran: $result"

    # A big-endian target that silently built little-endian would pass a
    # naive "did it run" check, so the program reports what it observed.
    local want_endian; want_endian="$(arch_field "$arch" endian)"
    case "$result" in
        *"endian=$want_endian"*) ;;
        *) die "expected $want_endian-endian, got: $result" ;;
    esac
    local want_bits; want_bits="$(arch_field "$arch" bits)"
    case "$result" in
        *"bits=$want_bits"*) ;;
        *) die "expected ${want_bits}-bit, got: $result" ;;
    esac

    # Some CPUs lack instruction classes the compiler would otherwise be free
    # to emit -- e500v2 has no classic FPU, so a hard-float build produces a
    # binary that disassembles fine, runs under a permissive emulator, and dies
    # with SIGILL on the real board. Emulation does not catch that; auditing the
    # instruction stream does.
    local forbidden
    forbidden="$(arch_field "$arch" forbidden_insn_re)"
    if [ -n "$forbidden" ]; then
        local od hits
        od="toolchain/$arch/bin/$triple-objdump"
        hits="$("$od" -d "$out/hello" 2>/dev/null \
                | awk -F"\t" 'NF>=3 {split($3,a," "); print a[1]}' \
                | grep -cE "$forbidden" || true)"
        if [ "${hits:-0}" -ne 0 ]; then
            die "$hits instruction(s) this CPU cannot execute. Pattern: $forbidden
     A hard-float or wrong-CPU build gets this far and then SIGILLs on hardware."
        fi
        echo "  instruction audit: 0 forbidden (pattern: $forbidden)"
    fi

    # The glibc ABI floor. A binary built against a newer minimum kernel aborts
    # at startup with "kernel too old" -- it does not degrade, it refuses. This
    # was a real regression: the aarch64 and powerpc samples default to
    # AS_HEADERS and pinned themselves to 6.16.0, which would not have started
    # on the AS5610's 5.10/6.1 kernel. Documenting the intent was not enough.
    local abimax
    abimax="$(arch_field "$arch" abi_kernel_max)"
    if [ -n "$abimax" ]; then
        local got lowest
        got="$(file "$out/hello" | grep -oE 'for GNU/Linux [0-9.]+' | awk '{print $3}')"
        if [ -n "$got" ]; then
            lowest="$(printf '%s\n%s\n' "$got" "$abimax" | sort -V | head -1)"
            if [ "$lowest" != "$got" ]; then
                die "binary requires Linux >= $got, above this architecture's ceiling of $abimax.
     Check CT_GLIBC_KERNEL_VERSION_NONE in bootstrap/configs/$arch.defconfig.
     End-of-service-life boards run vendor kernels that will never be updated."
            fi
            echo "  abi floor: Linux $got (ceiling $abimax)"
        fi
    fi

    echo "  OK"
}

case "${1:-}" in
    seed)  [ $# -eq 2 ] || die "usage: $0 seed <arch>";  cmd_seed  "$2" ;;
    build) [ $# -eq 2 ] || die "usage: $0 build <arch>"; cmd_build "$2" ;;
    test)  [ $# -eq 2 ] || die "usage: $0 test <arch>";  cmd_test  "$2" ;;
    *) die "usage: $0 {seed|build|test} <arch>" ;;
esac
