#!/usr/bin/env bash
# Выпуск релиза прошивки на GitHub (D-038): сборка, манифест, тег vX.Y.Z с файлами
# firmware.bin и manifest.json. Устройство берет последний релиз (не черновик и не
# pre-release): https://github.com/4ap4ap4mak/PLC_price_OPTA/releases/latest
#
#   scripts/release.sh X.Y.Z [файл-с-описанием.md]
#
# Требования: чистое рабочее дерево, коммит уже в origin, версия в src/FirmwareVersion.h
# равна X.Y.Z, pio и gh в PATH (docs/DEVELOPMENT.md). Без файла описания — описание из
# коммитов (--generate-notes).
set -euo pipefail

die() { echo "release: $*" >&2; exit 1; }

[ $# -ge 1 ] || die "укажите версию: scripts/release.sh X.Y.Z [notes.md]"
version=$1
notes=${2:-}
[[ $version =~ ^[0-9]{1,4}\.[0-9]{1,4}\.[0-9]{1,4}$ ]] || die "версия должна быть X.Y.Z: $version"

cd "$(git rev-parse --show-toplevel)"

source_version=$(sed -n 's/.*kFirmwareVersion = "\([^"]*\)".*/\1/p' src/FirmwareVersion.h)
[ "$source_version" = "$version" ] || \
    die "в src/FirmwareVersion.h версия '$source_version', а не '$version'"

git diff --quiet && git diff --cached --quiet || die "есть незакоммиченные изменения"
commit=$(git rev-parse HEAD)
git fetch --quiet origin
[ -n "$(git branch -r --contains "$commit")" ] || die "коммит $commit не отправлен в origin"
if gh release view "v$version" >/dev/null 2>&1; then
    die "релиз v$version уже есть"
fi

pio run -e opta
bin=.pio/build/opta/firmware.bin
size=$(wc -c < "$bin" | tr -d ' ')
sha=$(sha256sum "$bin" | cut -d' ' -f1)
# Пределы — как в lib/fwupdate (kMinImageSize…kMaxImageSize)
[ "$size" -ge 65536 ] && [ "$size" -le 786432 ] || die "размер $size вне 64 КБ…768 КБ"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cp "$bin" "$out/firmware.bin"
printf '{"board":"opta","version":"%s","size":%s,"sha256":"%s"}\n' \
    "$version" "$size" "$sha" > "$out/manifest.json"
cat "$out/manifest.json"

if [ -n "$notes" ]; then
    notes_args=(--notes-file "$notes")
else
    notes_args=(--generate-notes)
fi
gh release create "v$version" --target "$commit" --title "v$version" "${notes_args[@]}" \
    "$out/firmware.bin" "$out/manifest.json"
echo "release: v$version ($size байт, sha256 $sha) опубликован"
