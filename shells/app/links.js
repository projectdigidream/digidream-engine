//
// A web page the studio asks the system's browser to open (its "a new
// version is out" notice): https, with a host, and nothing else -- no file:,
// no custom schemes, no javascript:. The Android and iOS shells hold the same
// rule (MainActivity.java, Shell.swift: openUrl); test/open_url.test.js holds
// all three to it.
//
/** The address to open, or null when it is not one the shell will open. */
export function externalUrl (url) {
  if (typeof url !== 'string' || !url.startsWith('https://')) return null
  try {
    const u = new URL(url)
    return u.protocol === 'https:' && u.hostname ? u.href : null
  } catch { return null }
}
