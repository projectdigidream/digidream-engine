//
// The iOS shell's half of `window.dnpNative`, injected before any page script.
//
// android/web/shell.js is staged on iOS UNCHANGED (scripts/build_ios.sh copies
// it, as build_apk.sh does): it talks to `window.dnpNative`, which on Android
// is a Java object and here is this file -- the same four names over
// WKScriptMessageHandlerWithReply. One shell.js for both phones, so the link
// relay and the studio's calls have one implementation on the page side.
//
// Every call is a message to the "dnp" handler with an `op`; Swift answers
// through the reply handler, which postMessage returns as a promise. The
// studio's calls come back to the page through window.__dnpStudioReply, which
// shell.js defines, exactly as MainActivity.reply does on Android.
//
(function () {
  'use strict'
  const post = (msg) => window.webkit.messageHandlers.dnp.postMessage(msg)
  window.dnpNative = {
    up: (b64) => { post({ op: 'up', data: b64 }).catch(() => {}) },
    close: () => { post({ op: 'close' }).catch(() => {}) },
    log: (line) => { post({ op: 'log', line: String(line) }).catch(() => {}) },
    studio: (id, op, argsJson) => {
      post({ op: 'studio', id, call: op, args: argsJson })
        .then((r) => window.__dnpStudioReply && window.__dnpStudioReply(id, true, r === undefined ? null : r))
        .catch((e) => window.__dnpStudioReply && window.__dnpStudioReply(id, false, String(e && e.message || e)))
    }
  }
})()
