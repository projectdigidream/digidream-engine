#!/usr/bin/env bash
#
# A macOS .app from an app staged by build_app.sh, without electron-builder.
#
#   scripts/build_app.sh --module starter --suite --out build/app-suite
#   (cd build/app-suite/starter && npm install)        # once: fetches Electron
#   scripts/mac_app.sh build/app-suite/starter "Digidream Suite"
#
# The staged folder goes into a copy of its own Electron.app as
# Contents/Resources/app -- the layout it already runs from with `npm start`
# (bin/, modules/, assets/ beside app/main.js). electron-builder's layout puts
# those in Resources/ instead, where main.js does not look.
#
# Signed ad hoc: it runs on this Mac and on one that clears the quarantine
# (right-click -> Open, once). Not notarized -- that needs an Apple Developer
# account. Writes <staged>/../<Name>.app and a .zip of it beside.
#
set -euo pipefail
SRC="$(cd "${1:?usage: mac_app.sh <staged app dir> [name]}" && pwd)"
NAME="${2:-$(node -e "console.log(require('$SRC/package.json').productName || 'Digidream')")}"
[[ "$(uname -s)" == Darwin ]] || { echo "[mac] macOS only" >&2; exit 2; }
EL="$SRC/node_modules/electron/dist/Electron.app"
[[ -d "$EL" ]] || { echo "[mac] no Electron in $SRC: run (cd $SRC && npm install)" >&2; exit 2; }
ID=$(node -e "console.log(require('$SRC/package.json').build?.appId || 'dev.dnp.app')")
OUTDIR="$(cd "$SRC/.." && pwd)"
APP="$OUTDIR/$NAME.app"

rm -rf "$APP"
cp -R "$EL" "$APP"
R="$APP/Contents/Resources"
rm -f "$R/default_app.asar"
mkdir -p "$R/app"
# Everything staged but Electron itself and old packaging output -- at the TOP
# only (rsync's leading /): the wasm builds and plugins have dist/ folders of
# their own, and bsdtar's --exclude ./dist dropped every one of them.
rsync -a --exclude /node_modules --exclude /dist "$SRC/" "$R/app/"

# Its own name in package.json too: Electron names the app's data folder
# (~/Library/Application Support/<name>) after it, and two apps staged from
# one module would otherwise share one -- the same store, the same game tree.
node -e "const f='$R/app/package.json',p=JSON.parse(require('fs').readFileSync(f));p.name=p.productName=process.argv[1];require('fs').writeFileSync(f,JSON.stringify(p,null,2))" "$NAME"

PL="$APP/Contents/Info.plist"
plutil -replace CFBundleName -string "$NAME" "$PL"
plutil -replace CFBundleDisplayName -string "$NAME" "$PL"
plutil -replace CFBundleIdentifier -string "$ID" "$PL"

# Ad hoc, every nested bundle and the app (Electron ships linker-signed only,
# which `codesign --verify --strict` calls damaged once Resources change).
codesign --force --deep --sign - "$APP" 2>/dev/null
codesign --verify --deep --strict "$APP"

( cd "$OUTDIR" && rm -f "$NAME.zip" && ditto -c -k --keepParent "$NAME.app" "$NAME.zip" )
echo "[mac] $APP  ($(du -sh "$APP" | cut -f1)), and $OUTDIR/$NAME.zip ($(du -sh "$OUTDIR/$NAME.zip" | cut -f1))"
