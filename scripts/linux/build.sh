#!/usr/bin/env bash
# Cross-compiles BC250Kmd.sys on Linux with clang-cl + lld-link.
# Headers/libs: NuGet WDK+SDK in packages/ and the MSVC headers copied into toolchain/msvc/include.
# Output: build/<cfg>/BC250Kmd.sys (UNSIGNED) + BC250Kmd.pdb. Signing/catalog happen on Windows (see deploy.sh).
#
# Usage: scripts/linux/build.sh [Release|Debug] [kmd|display]
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
CFG=${1:-Release}
TARGET=${2:-kmd}

V=10.0.26100.0
WDK=$ROOT/packages/Microsoft.Windows.WDK.x64.10.0.26100.6584/c
SDK=$ROOT/packages/Microsoft.Windows.SDK.CPP.10.0.26100.6584/c
MSVC_INC=$ROOT/toolchain/msvc/include

case $TARGET in
  kmd)     PROJ=BC250Kmd.vcxproj;     NAME=BC250Kmd;     EXTRA_DEF=(/DBC250_KMD) ;;
  display) PROJ=BC250Display.vcxproj; NAME=BC250Display; EXTRA_DEF=() ;;
  *) echo "target must be kmd|display" >&2; exit 2 ;;
esac

OUT=$ROOT/build/$CFG/$TARGET
mkdir -p "$OUT/obj"

# Source list straight from the vcxproj so both builds stay in sync.
mapfile -t SRCS < <(grep -oP '<ClCompile Include="\K[^"]+' "$PROJ" | tr '\\' '/')

COMMON=(
  --target=x86_64-pc-windows-msvc /nologo /c /TP /std:c++17
  /D_AMD64_ /DAMD64 /D_WIN64 /DNTDDI_VERSION=0x0A00000B /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DWINNT=1
  /DDEPRECATE_DDK_FUNCTIONS=1 /DPOOL_NX_OPTIN=1 /D_KERNEL_MODE /DWIN32_LEAN_AND_MEAN "${EXTRA_DEF[@]}"
  /GS- /Gy /Gw /Zc:wchar_t /fp:precise /EHs-c- /GR- /W3
  -Wno-nonportable-include-path -Wno-microsoft-enum-forward-reference -Wno-unknown-pragmas -Wno-pragma-pack -Wno-ignored-pragma-intrinsic
  /imsvc "$WDK/Include/$V/km" /imsvc "$WDK/Include/$V/km/crt" /imsvc "$WDK/Include/$V/shared"
  /imsvc "$SDK/Include/$V/shared" /imsvc "$SDK/Include/$V/um" /imsvc "$WDK/Include/$V/um" /imsvc "$MSVC_INC"
  /Isrc /Isrc/gpu /Isrc/kmd
)
if [[ $CFG == Debug ]]; then COMMON+=(/Od /Zi /DDBG=1 /D_DEBUG); else COMMON+=(/O2 /Zi /DNDEBUG); fi

# Case-insensitive include lookup (Windows headers + project sources assume it).
python3 scripts/linux/gen-vfs.py build/vfs.yaml \
  "$WDK/Include/$V/km" "$WDK/Include/$V/km/crt" "$WDK/Include/$V/shared" \
  "$SDK/Include/$V/shared" "$SDK/Include/$V/um" "$WDK/Include/$V/um" "$MSVC_INC" src src/gpu src/kmd
COMMON+=(-Xclang -ivfsoverlay -Xclang "$ROOT/build/vfs.yaml")

OBJS=()
pids=()
for s in "${SRCS[@]}"; do
  [[ $s == *.cxx || $s == *.cpp || $s == *.c ]] || continue
  o=$OUT/obj/$(echo "${s%.*}" | tr '/' '_').obj
  OBJS+=("$o")
  clang-cl "${COMMON[@]}" "$s" "/Fo$o" &
  pids+=($!)
done
fail=0; for p in "${pids[@]}"; do wait "$p" || fail=1; done
((fail==0)) || { echo "COMPILE FAILED" >&2; exit 1; }

LIB=$WDK/Lib/$V/km/x64
# NOTE: libs are looked up case-insensitively on Windows, so list them by exact on-disk name.
LIBS=(ntoskrnl.lib bufferoverflowfastfailk.lib libcntpr.lib wdm.lib)
[[ $TARGET == kmd || $TARGET == display ]] && LIBS+=(displib.lib)

lld-link /nologo /DRIVER /SUBSYSTEM:NATIVE,10.0 /TSAWARE:NO /NODEFAULTLIB /ENTRY:GsDriverEntry /MACHINE:X64 \
  /RELEASE /DEBUG /PDB:"$OUT/$NAME.pdb" /OUT:"$OUT/$NAME.sys" \
  /MERGE:_TEXT=.text /MERGE:_PAGE=PAGE /NXCOMPAT /DYNAMICBASE /OPT:REF /OPT:ICF \
  /STACK:0x40000,0x1000 /VERSION:10.0 /OSVERSION:10.0 /SECTION:INIT,d \
  /LIBPATH:"$LIB" "${LIBS[@]}" "${OBJS[@]}"

ls -l "$OUT/$NAME.sys"
