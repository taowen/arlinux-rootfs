#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cache_root="${ARLINUX_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/arlinux}"
cache_dir="${ARLINUX_LINUX_CACHE:-$cache_root/glibc}"
glibc_version="${BIONICX_GLIBC_VERSION:-2.41}"
recipe="$repo_dir/runtime/glibc/$glibc_version"
test -f "$recipe/recipe.env" || { echo "missing glibc recipe: $glibc_version" >&2; exit 2; }
source "$recipe/recipe.env"
# Paths compiled into the Termux-derived glibc are namespace paths. The
# syscall boundary resolves this neutral prefix using BIONICX_ROOTFS before
# reaching Android; no Android package name belongs in these binaries.
source_prefix=/arlinux-rootfs
jobs="${BIONICX_GLIBC_JOBS:-8}"
# Standard distribution directories must work before the ldconfig trigger runs.
# Keep them in the loader fallback search, after --library-path and the cache.
library_dirs="${BIONICX_GLIBC_LIBRARY_DIRS:-}"
trusted_dirs=""
IFS=: read -r -a distribution_dirs <<< "$library_dirs"
for dir in "${distribution_dirs[@]}"; do
    [[ "$dir" =~ ^[a-zA-Z0-9_-]+(/[a-zA-Z0-9_-]+)*$ ]] || {
        echo "invalid relative library directory: $dir" >&2
        exit 2
    }
    trusted_dirs+=" $source_prefix/$dir"
done

verify_output() {
    python3 - "$1" "$source_prefix" <<'PY'
from pathlib import Path
import sys
import subprocess

output = Path(sys.argv[1])
prefix = sys.argv[2].encode()
libc = (output / "libc.so.6").read_bytes()
symbols = subprocess.check_output(["readelf", "--dyn-syms", "--wide", str(output / "libc.so.6")], text=True)
if "__vsyslog_chk@@GLIBC_2.17" not in symbols:
    raise SystemExit("glibc contract: fortified vsyslog ABI is not exported")
expected_resolver = prefix + b"/etc/resolv.conf"
if expected_resolver not in libc:
    raise SystemExit("glibc contract: fixed rootfs resolver path is absent")
for name in ("libc.so.6", "ld-linux-aarch64.so.1", "ldconfig"):
    binary = (output / name).read_bytes()
    if b"io.taowen.arlinux" in binary or b"/data/user/0/" in binary:
        raise SystemExit(f"glibc contract: Android package path in {name}")

# Android app seccomp traps these calls even before glibc can observe ENOSYS.
# The pinned source recipe must compile them out; runtime instruction rewriting
# is intentionally not an execution path.
svc = bytes.fromhex("010000d4")
clone3 = bytes.fromhex("683680d2")
robust = bytes.fromhex("680c80d2")
if clone3 + svc in libc:
    raise SystemExit("glibc contract: raw clone3 stub remains")
for offset in range(0, max(0, len(libc) - 16), 4):
    if libc[offset:offset + 4] == robust and svc in libc[offset + 4:offset + 16]:
        raise SystemExit("glibc contract: raw set_robust_list stub remains")
PY
}

mkdir -p "$cache_dir"
definition_hash="$({
    printf '%s\n' "$glibc_version" "$glibc_sha256" "$package_commit" \
        "$source_prefix" "$library_dirs"
    find "$recipe" "$repo_dir/runtime/glibc/common" -maxdepth 1 -type f -print0 \
      | sort -z | xargs -0 sha256sum | cut -d ' ' -f1
    sha256sum "$0" | cut -d ' ' -f1
} | sha256sum | cut -c1-16)"
result_dir="$cache_dir/android-glibc-$definition_hash"
exec 9>"$cache_dir/android-glibc-$definition_hash.lock"
flock 9
if [[ -x "$result_dir/output/ld-linux-aarch64.so.1" \
        && -f "$result_dir/output/libc.so.6" \
        && -f "$result_dir/output/libm.so.6" \
        && -x "$result_dir/output/ldconfig" ]]; then
    verify_output "$result_dir/output"
    echo "$result_dir/output"
    exit 0
fi

archive="$cache_dir/glibc-$glibc_version.tar.xz"
if ! echo "$glibc_sha256  $archive" | sha256sum -c - >/dev/null 2>&1; then
    rm -f "$archive" "$archive.part"
    gnu_mirror="${ARLINUX_GNU_MIRROR:-https://mirrors.tuna.tsinghua.edu.cn/gnu}"
    curl -fL --retry 2 "$gnu_mirror/glibc/glibc-$glibc_version.tar.xz" \
        -o "$archive.part"
    mv "$archive.part" "$archive"
fi
echo "$glibc_sha256  $archive" | sha256sum -c -

package_repo="$cache_dir/glibc-packages"
if [[ ! -d "$package_repo/.git" ]]; then
    git clone https://github.com/termux-pacman/glibc-packages.git "$package_repo"
fi
if ! git -C "$package_repo" cat-file -e "$package_commit^{commit}" 2>/dev/null; then
    git -C "$package_repo" fetch --depth 1 origin "$package_commit"
fi
actual_commit="$(git -C "$package_repo" rev-parse "$package_commit^{commit}")"
[[ "$actual_commit" == "$package_commit" ]] || {
    echo "unexpected glibc-packages commit: $actual_commit" >&2
    exit 1
}

temporary="$(mktemp -d "$cache_dir/android-glibc-$definition_hash.XXXXXXXX")"
cleanup() {
    case "$temporary" in
        "$cache_dir"/android-glibc-*) rm -rf -- "$temporary" ;;
        *) echo "refusing to clean unexpected path: $temporary" >&2 ;;
    esac
}
trap cleanup EXIT
mkdir -p "$temporary/source" "$temporary/package" "$temporary/build" \
    "$temporary/output"
tar -xJf "$archive" -C "$temporary/source" --strip-components=1
git -C "$package_repo" archive "$package_commit" gpkg/glibc | \
    tar -x -C "$temporary/package" --strip-components=2

apply_source_patch() {
    local patch_file="$1"
    sed -e "s|@TERMUX_PREFIX_CLASSICAL@|$source_prefix|g" \
        -e "s|@TERMUX_PREFIX@|$source_prefix|g" "$patch_file" | \
        patch -d "$temporary/source" -p1 --forward --batch
}
for patch_file in "$temporary/package"/*.patch; do
    apply_source_patch "$patch_file"
done
apply_source_patch \
    "$recipe/zz-bionicx-robust-fallback.patch"

apply_source_patch "$recipe/zz-arlinux-ldconfig-prefix.patch"
apply_source_patch "$recipe/zz-arlinux-loader-search-path.patch"

linux_dir="$temporary/source/sysdeps/unix/sysv/linux"
cp "$temporary/package"/syscall.c \
    "$temporary/package"/fakesyscall*.h \
    "$temporary/package"/fake_epoll_pwait2.c \
    "$temporary/package"/setfs{u,g}id.c "$linux_dir/"
# Keep upstream mprotect: the imported PROT_EXEC fallback corrupts its
# /proc/self/maps buffers and can replace mappings after an EACCES failure.
# Callers (including Qt QML) must receive the kernel permission error.
# One stateful SysV SHM implementation lives in libc. Do not build the imported
# ashmem implementation as well, and do not override these APIs in LD_PRELOAD.
cp "$repo_dir/runtime/glibc/common/sysv-shm.c" "$temporary/source/sysvipc/shm-runtime.c"
cp "$repo_dir/runtime/glibc/common/sysv-semaphore.c" "$temporary/source/sysvipc/sem-runtime.c"
cp "$repo_dir/runtime/glibc/common/close_range.c" "$temporary/source/io/close_range.c"
python3 - "$temporary/source/sysvipc/Makefile" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
old = 'shmem-android \\\n\t    shmat shmdt shmget shmctl'
if text.count(old) != 1:
    raise SystemExit('glibc SysV IPC routine list changed; review the source recipe')
text = text.replace(old, 'shm-runtime')
old = 'semop semget semctl semtimedop'
if text.count(old) != 1:
    raise SystemExit('glibc semaphore routine list changed; review the source recipe')
path.write_text(text.replace(old, 'sem-runtime'))
PY
cp "$temporary/package/syslog.c" "$temporary/source/misc/"
if [[ -f "$recipe/zz-arlinux-syslog-export.patch" ]]; then
    apply_source_patch "$recipe/zz-arlinux-syslog-export.patch"
fi
mv "$linux_dir/aarch64/clone3.S" "$linux_dir/aarch64/clone3.S.disabled"
mv "$linux_dir/aarch64/syscall.S" "$linux_dir/aarch64/syscallS.S"

cp "$temporary/package"/android_passwd_group.{c,h} \
    "$temporary/package"/android_system_user_ids.h "$temporary/source/nss/"
apply_source_patch \
    "$recipe/zz-android-group-members.patch"
bash "$temporary/package/gen-android-ids.sh" "$source_prefix" \
    "$temporary/source/nss/android_ids.h" \
    "$temporary/package/android_system_user_ids.h"

disabled_header="$linux_dir/aarch64/disabled-syscall.h"
# The same libc backend owns public IPC calls and syscall(2). Replace the
# imported ENOSYS entries rather than repairing them later with LD_PRELOAD.
python3 - "$temporary/package/fakesyscall.json" "$linux_dir/fakesyscall-base.h" <<'PY'
from pathlib import Path
import json
import sys
path = Path(sys.argv[1])
policy = json.loads(path.read_text())
calls = {
    'semget(a0, a1, a2)': 'semget',
    'semop(a0, (struct sembuf *)a1, a2)': 'semop',
    'semtimedop(a0, (struct sembuf *)a1, a2, (const struct timespec *)a3)': 'semtimedop',
    'semctl(a0, a1, a2, (union arlinux_semun){ .bits = (unsigned long)a3 })': 'semctl',
}
for expression, name in calls.items():
    for names in policy.values():
        if name in names:
            names.remove(name)
    policy[expression] = [name]
path.write_text(json.dumps(policy))
header = Path(sys.argv[2])
header.write_text(header.read_text() + '\n#include <sys/sem.h>\nunion arlinux_semun { int val; unsigned short *array; struct semid_ds *buf; unsigned long bits; };\n')
PY
touch "$disabled_header"
while read -r syscall_name; do
    grep "#define __NR_${syscall_name} " \
        "$linux_dir/aarch64/arch-syscall.h" >> "$disabled_header" || true
    sed -i "/#define __NR_${syscall_name} /d" \
        "$linux_dir/aarch64/arch-syscall.h"
done < <(jq -r '.[] | .[]' "$temporary/package/fakesyscall.json")
{
    printf '\n#define DISABLED_SYSCALL_WITH_FAKESYSCALL \\\n'
    while IFS= read -r fake; do
        need_return=false
        while IFS= read -r syscall_name; do
            if grep -q "#define __NR_${syscall_name} " "$disabled_header"; then
                printf '\tcase __NR_%s: \\\n' "$syscall_name"
                need_return=true
            elif [[ "$syscall_name" =~ ^[0-9]+$ ]]; then
                printf '\tcase %s: \\\n' "$syscall_name"
                need_return=true
            fi
        done < <(jq -r --arg fake "$fake" '.[$fake][]' \
            "$temporary/package/fakesyscall.json")
        if [[ "$need_return" == true ]]; then
            printf '\t\treturn %s; \\\n' "$fake"
        fi
    done < <(jq -r 'keys[]' "$temporary/package/fakesyscall.json")
} >> "$disabled_header"
sed -i '$ s| \\$||' "$disabled_header"

python3 "$repo_dir/runtime/glibc/common/install-syscalls.py" "$temporary/source"

printf '%s\n' \
    "user-defined-trusted-dirs=$trusted_dirs" \
    "slibdir=$source_prefix/lib" \
    "rtlddir=$source_prefix/lib" \
    "sbindir=$source_prefix/bin" \
    "rootsbindir=$source_prefix/bin" > "$temporary/build/configparms"

pushd "$temporary/build" >/dev/null
if command -v ccache >/dev/null 2>&1; then
    export CC="ccache aarch64-linux-gnu-gcc"
    export CXX="ccache aarch64-linux-gnu-g++"
    export CCACHE_BASEDIR="$temporary" CCACHE_NOHASHDIR=true
fi
"$temporary/source/configure" \
    --prefix="$source_prefix" --libdir="$source_prefix/lib" \
    --libexecdir="$source_prefix/lib" \
    --host=aarch64-linux-gnu --build=x86_64-linux-gnu \
    --enable-kernel=3.7 --enable-bind-now --disable-multi-arch \
    --enable-stack-protector=strong --disable-nscd --disable-profile \
    --disable-werror --disable-default-pie
make --silent -j"$jobs"
popd >/dev/null

cp "$temporary/build/elf/ldconfig" "$temporary/output/ldconfig"
cp "$temporary/build/libc.so" "$temporary/output/libc.so.6"
cp "$temporary/build/elf/ld.so" "$temporary/output/ld-linux-aarch64.so.1"
cp "$temporary/build/math/libm.so" "$temporary/output/libm.so.6"
aarch64-linux-gnu-strip --strip-unneeded \
    "$temporary/output/libc.so.6" \
    "$temporary/output/ld-linux-aarch64.so.1" \
    "$temporary/output/libm.so.6" \
    "$temporary/output/ldconfig"
verify_output "$temporary/output"

printf '%s\n' \
    "glibc=$glibc_version" \
    "glibc_sha256=$glibc_sha256" \
    "glibc_packages_commit=$package_commit" \
    "build_definition=$definition_hash" > "$temporary/output/BUILD-INFO"

mv "$temporary" "$result_dir"
trap - EXIT
echo "$result_dir/output"
