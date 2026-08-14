#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST="$ROOT/dist"
NAME="POC"
ARCHIVE="$DIST/$NAME.tar.gz"
ZIP="$DIST/$NAME.zip"
UNPACKED="$DIST/$NAME"
STAGING="$(mktemp -d "${TMPDIR:-/tmp}/topssim-campaign.XXXXXX")"
OPEN5GS_STAGING="$STAGING/$NAME/open5gs"

mkdir -p "$DIST"
rm -rf "$UNPACKED" "$ARCHIVE" "$ZIP"

cleanup() {
  rm -rf "$STAGING"
}
trap cleanup EXIT

mkdir -p "$STAGING/$NAME"

rsync -a \
  --exclude=".git" \
  --exclude=".DS_Store" \
  --exclude="*/.DS_Store" \
  --exclude="build" \
  --exclude="install" \
  --exclude="logs" \
  --exclude="dist" \
  --exclude="node_modules" \
  --exclude="*/node_modules" \
  --exclude="__pycache__" \
  --exclude="*/__pycache__" \
  --exclude=".pycache-check" \
  --exclude="*.pyc" \
  --exclude="*.pcap" \
  --exclude="webui/.env" \
  --exclude="topssim/tools/user_profile_campaign.env" \
  --exclude="topssim/tools/user_profile_campaign.cloud-mini.env" \
  --exclude="topssim/tools/user_profile_campaign.cloud-mini-pdu.env" \
  --exclude="topssim/tools/user_profile_campaign.pdu-20.env" \
  "$ROOT/" "$OPEN5GS_STAGING/"

cp "$ROOT/topssim/handoff/README.md" "$STAGING/$NAME/README.md"
cp "$ROOT/topssim/handoff/user_profile_campaign.cloud.env.template" \
  "$STAGING/$NAME/user_profile_campaign.cloud.env.template"
cp "$ROOT/topssim/tools/run_user_profile_campaign.sh" \
  "$STAGING/$NAME/run_user_profile_campaign.sh"

mkdir -p "$STAGING/$NAME/config"
cp "$ROOT/topssim/handoff/config/README.md" "$STAGING/$NAME/config/README.md"

mv "$STAGING/$NAME" "$UNPACKED"
tar -czf "$ARCHIVE" -C "$DIST" "$NAME"
(cd "$DIST" && zip -qr "$ZIP" "$NAME")

printf 'Wrote %s\n' "$ARCHIVE"
printf 'Wrote %s\n' "$ZIP"
printf 'Wrote %s\n' "$UNPACKED"
printf '\nSuggested message:\n'
printf '  See README.md after extracting the archive.\n'
