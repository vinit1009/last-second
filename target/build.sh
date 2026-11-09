#!/bin/bash
# Builds Last Second for QNX OS 8.0 (x86_64) and a QEMU image that runs the demo at boot.
# Run on a Linux x86_64 host with QNX SDP 8.0 (see the Deadline CI codelab for a Mac):
#   source ~/qnx800/qnxsdp-env.sh
#   target/build.sh [image-dir]           # default image dir: ../last-second-image
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${1:-$REPO/../last-second-image}
: "${QNX_TARGET:?source qnxsdp-env.sh first}"

OUT=$REPO/build/x86_64
mkdir -p "$OUT" "$IMAGE"
CFLAGS=(-Vgcc_ntox86_64 -O2 -Wall -Wextra -Werror -std=gnu11 -I"$REPO/include" -I"$REPO/src")
qcc "${CFLAGS[@]}" -o "$OUT/blackboxd" "$REPO/src/blackboxd.c" "$REPO/src/core.c"
for p in pump hog bbtool; do
  qcc "${CFLAGS[@]}" -o "$OUT/$p" "$REPO/src/$p.c"
done

# mkqnximage reads local/snippets from the image directory; point the file list at this checkout.
rm -rf "$IMAGE/local"
mkdir -p "$IMAGE/local/snippets"
sed "s|/work/last-second|$REPO|g" "$REPO/image/local/snippets/system_files.custom" > "$IMAGE/local/snippets/system_files.custom"
cp "$REPO/image/local/snippets/post_start.custom" "$IMAGE/local/snippets/"
cd "$IMAGE"
# SECPOL=open boots with secpolgenerate recording the rules the system needs; SECPOL=secure
# enforces POLICY (a text policy, compiled by mkqnximage). Default: no security policy.
SECPOL_ARGS=()
case "${SECPOL:-no}" in
  no) ;;
  open) SECPOL_ARGS=(--secpol=open ${POLICY:+--policy=$POLICY}) ;;
  secure) SECPOL_ARGS=(--secpol=secure --policy="${POLICY:?set POLICY to a text policy for a secure image}") ;;
  *) echo "SECPOL must be no, open or secure" >&2; exit 2 ;;
esac
echo "${SECPOL:-no}" > "$REPO/build/x86_64/secpol-mode"
mkqnximage --type=qemu --arch=x86_64 --cpu=1 --ram=1G --hostname=last-second "${SECPOL_ARGS[@]}" --noprompt --force --build
echo "Image ready in $IMAGE/output. Next, on the host with QEMU: target/run-qemu.sh $IMAGE"
