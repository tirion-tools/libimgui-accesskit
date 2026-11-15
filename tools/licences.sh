#!/usr/bin/env bash
# Licence check (gate job `licence`, in the gate container). Usage: tools/licences.sh <executable>...
#  1. LICENSE is the MIT licence.
#  2. The dependencies compiled or linked statically into a consumer carry the licences the README
#     declares: Dear ImGui (MIT), SDL3 (zlib), AccessKit C (MIT or Apache-2.0).
#  3. Every shared library the given executables load belongs to a Debian package listed in
#     tools/licences.allow (package, then its licence as stated in the package's copyright file).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
rc=0
fail() {
  echo "licence: $*"
  rc=1
}

grep -qx 'MIT License' LICENSE && grep -q 'Permission is hereby granted, free of charge' LICENSE \
  || fail "LICENSE is not the MIT licence"

grep -q 'The MIT License (MIT)' "${IMGUI_DIR:?}/LICENSE.txt" || fail "Dear ImGui: LICENSE.txt is not MIT"
grep -q "This software is provided 'as-is'" /usr/local/share/licenses/SDL3/LICENSE.txt || fail "SDL3: not zlib"
grep -q 'Permission is hereby granted, free of charge' /opt/accesskit-c/LICENSE-MIT \
  && grep -q 'Apache License' /opt/accesskit-c/LICENSE-APACHE || fail "AccessKit C: not MIT/Apache-2.0"

declare -A allowed
while read -r pkg licence; do
  [[ -z $pkg || $pkg == \#* ]] || allowed[$pkg]=$licence
done < tools/licences.allow
[[ $# -gt 0 ]] || fail "no executables given"
for exe; do [[ -x $exe ]] || fail "$exe: no such executable"; done
declare -A seen
while read -r lib; do
  owner=$(dpkg -S "$(readlink -f "$lib")" 2>/dev/null | head -1 | cut -d: -f1)
  if [[ -z $owner ]]; then
    fail "$lib: not from a Debian package"
  elif [[ -z ${allowed[$owner]:-} ]]; then
    fail "$lib: package $owner is not in tools/licences.allow"
  elif [[ -z ${seen[$owner]:-} ]]; then
    seen[$owner]=1
    echo "$owner: ${allowed[$owner]}"
  fi
done < <(for exe; do ldd "$exe" || fail "ldd $exe"; done | awk '$2 == "=>" && $3 ~ /^\// {print $3}' | sort -u)
exit $rc
