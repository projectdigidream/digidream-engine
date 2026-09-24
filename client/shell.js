//
// The seam a native shell hooks into, and nothing at all in a browser.
//
// client/src/net_local.js picks its transport by asking whether `window.dnp`
// exists: present means "the game server is in this process tree, talk to it
// over the shell's pipe", absent means WebTransport to a real server over a
// real network. That check happens at MODULE SCOPE in main.js, so whatever
// sets `window.dnp` has to have run before main.js does.
//
// Electron has a hook for that -- app/preload.cjs runs before the page -- and
// a WebView does not, at least not without AndroidX. So the page declares the
// seam instead: this file is a CLASSIC script and main.js is a MODULE, and the
// HTML spec runs classic scripts in order before deferred module scripts. One
// line in index.html, and a shell that wants the hook replaces this file.
//
// IN A BROWSER THIS DOES NOTHING, and that is its whole job here. It is not a
// stub waiting to be filled in: a browser client talks to a server over
// WebTransport and must not have a local bridge, so the correct content of
// this file for the web is nothing at all.
//
// WHO REPLACES IT:
//
//   android/web/shell.js   staged over this one by scripts/build_apk.sh
//
// The desktop build does not: Electron's preload gets there first and this
// file runs afterwards, finds `window.dnp` already set, and leaves it alone.
// That is why this is empty rather than an assignment -- an empty file cannot
// overwrite what the preload just published.
