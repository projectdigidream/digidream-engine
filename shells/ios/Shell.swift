//
// The iOS shell: a web view with the client in it, and the server beside it.
//
// THE SAME CLIENT THE BROWSER LOADS, the same shell.js Android runs, and the
// same server -- linked in rather than spawned, because iOS spawns nothing.
// This file is MainActivity.java said in Swift:
//
//   MainActivity.serve()            AssetHandler (a WKURLSchemeHandler on dnp://app/)
//   MainActivity.Bridge             the "dnp" script message handler, below
//   GameServer                      ServerHost (a thread) + Link (an NWConnection)
//   MainActivity.unpack()           seed(): Bundle/game -> Documents/game
//
// WHY A CUSTOM SCHEME. WebAssembly, ES modules and an AudioWorklet need a
// secure context, and file:// is not one. A scheme served by a
// WKURLSchemeHandler is (it is how Capacitor's capacitor://localhost works),
// and, like appassets.androidplatform.net, it cannot reach the network: every
// request is answered here and an unknown one is a 404.
//
// NOT BUILT ON THE MACHINE THIS WAS WRITTEN ON (no Xcode there); see
// docs/IOS.md for what scripts/build_ios.sh needs and what is still pending.
//
import UIKit
import WebKit
import Network

let ORIGIN = "dnp://app"
let TAG = "digidream"

// MARK: - the app

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
  var window: UIWindow?
  func application(_ app: UIApplication, didFinishLaunchingWithOptions opts: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
    let w = UIWindow(frame: UIScreen.main.bounds)
    w.rootViewController = ShellController()
    w.makeKeyAndVisible()
    window = w
    return true
  }
}

func log(_ s: String) { NSLog("[\(TAG)] %@", s) }

// MARK: - the game tree

/// Where the server reads and the studio writes: Documents/game, seeded from
/// the bundle's game/ when the build stamp changes. A module the user made is
/// never touched (app/studio_files.js has the same rule).
final class GameTree {
  let root: URL
  let bundled: URL
  init() {
    root = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("game")
    bundled = Bundle.main.resourceURL!.appendingPathComponent("game")
  }

  func seed() throws {
    let stamp = (Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "0") + "/" + (try? String(contentsOf: bundled.appendingPathComponent("stamp.txt"))) ?? ""
    let marker = root.appendingPathComponent("modules/.seeded")
    if (try? String(contentsOf: marker)) == stamp { return }
    let fm = FileManager.default
    try fm.createDirectory(at: root.appendingPathComponent("modules"), withIntermediateDirectories: true)
    for name in (try? fm.contentsOfDirectory(atPath: bundled.path)) ?? [] {
      let src = bundled.appendingPathComponent(name)
      let dst = root.appendingPathComponent(name)
      if name == "modules" {
        for m in (try? fm.contentsOfDirectory(atPath: src.path)) ?? [] {
          let md = dst.appendingPathComponent(m)
          try? fm.removeItem(at: md)
          try fm.copyItem(at: src.appendingPathComponent(m), to: md)
        }
      } else {
        try? fm.removeItem(at: dst)
        try fm.copyItem(at: src, to: dst)
      }
    }
    try stamp.write(to: marker, atomically: true, encoding: .utf8)
    log("seeded \(root.path)")
  }

  /// A path under the tree, or nil. Relative, forward slashes, never `..`.
  func file(_ rel: String) -> URL? {
    if rel.isEmpty || rel.hasPrefix("/") || rel.contains("\\") || rel.contains("\0") { return nil }
    for seg in rel.split(separator: "/", omittingEmptySubsequences: false) {
      if seg.isEmpty || seg == "." || seg == ".." { return nil }
    }
    return root.appendingPathComponent(rel)
  }

  /// The studio's modules: every directory under modules/ but the bundled one.
  func userModules(except bundledModule: String) -> [URL] {
    let dir = root.appendingPathComponent("modules")
    let names = ((try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []).sorted()
    return names.filter { $0 != bundledModule && !$0.hasPrefix(".") }.map { dir.appendingPathComponent($0) }
      .filter { (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true }
  }

  /// start.txt, the two or three lines the build wrote.
  func start() throws -> [String: String] {
    let text = try String(contentsOf: root.appendingPathComponent("start.txt"))
    var kv: [String: String] = [:]
    for raw in text.split(separator: "\n") {
      let line = raw.trimmingCharacters(in: .whitespaces)
      if line.isEmpty || line.hasPrefix("#") { continue }
      if let eq = line.firstIndex(of: "=") {
        kv[String(line[..<eq]).trimmingCharacters(in: .whitespaces)] = String(line[line.index(after: eq)...]).trimmingCharacters(in: .whitespaces)
      }
    }
    return kv
  }
}

// MARK: - the server, in-process

/// dnp_server_run on a thread (ios/dnp_bridge.h, server/src/embed.cpp).
final class ServerHost {
  private(set) var port: UInt16 = 0

  static func freePort() -> UInt16 {
    // Ask the kernel: bind 0, read the number, close. The same accepted race
    // the desktop shell documents.
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    defer { close(fd) }
    var addr = sockaddr_in()
    addr.sin_family = sa_family_t(AF_INET)
    addr.sin_port = 0
    addr.sin_addr.s_addr = inet_addr("127.0.0.1")
    var len = socklen_t(MemoryLayout<sockaddr_in>.size)
    let ok = withUnsafeMutablePointer(to: &addr) { p in
      p.withMemoryRebound(to: sockaddr.self, capacity: 1) { sp in
        bind(fd, sp, len) == 0 && getsockname(fd, sp, &len) == 0
      }
    }
    return ok ? UInt16(bigEndian: addr.sin_port) : 34500
  }

  func start(workDir: URL, modules: [URL], map: URL, props: URL) throws {
    stop()
    port = ServerHost.freePort()
    var args = ["dnp-server", "--port", String(port)]
    for m in modules { args += ["--module", m.path] }
    args += ["--map", map.path, "--spawn", "start", "--props", props.path]
    // The server resolves nothing against the working directory when every
    // path it is given is absolute, which they are; chdir anyway so a stray
    // relative open lands in the tree rather than in /.
    FileManager.default.changeCurrentDirectoryPath(workDir.path)
    log("starting server on 127.0.0.1:\(port) for \(map.lastPathComponent)")
    let cargs = args.map { strdup($0) }
    defer { cargs.forEach { free($0) } }
    let rc = cargs.withUnsafeBufferPointer { buf -> Int32 in
      let ptrs = buf.map { UnsafePointer<CChar>($0) }
      return ptrs.withUnsafeBufferPointer { dnp_embed_start(Int32(ptrs.count), $0.baseAddress) }
    }
    if rc != 0 { throw NSError(domain: TAG, code: Int(rc), userInfo: [NSLocalizedDescriptionKey: "the server is already running"]) }
  }

  func stop() { dnp_embed_stop() }
  var running: Bool { dnp_embed_running() != 0 }
}

// MARK: - the link

/// A byte pipe to the server's loopback socket. Whole frames are the page's
/// business (framing.js); this moves bytes.
final class Link {
  private var conn: NWConnection?
  private let queue = DispatchQueue(label: "dnp-link")
  var onBytes: ((Data) -> Void)?
  var onClosed: ((String) -> Void)?
  private var closed = false

  func connect(port: UInt16, timeout: TimeInterval, server: ServerHost, done: @escaping (Error?) -> Void) {
    let deadline = Date().addingTimeInterval(timeout)
    closed = false
    attempt(port: port, deadline: deadline, server: server, done: done)
  }

  private func attempt(port: UInt16, deadline: Date, server: ServerHost, done: @escaping (Error?) -> Void) {
    if !server.running {
      done(NSError(domain: TAG, code: 1, userInfo: [NSLocalizedDescriptionKey: "the game server stopped while starting (exit \(dnp_embed_exit_code())); see the system log"]))
      return
    }
    let c = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
    var settled = false
    c.stateUpdateHandler = { [weak self] state in
      guard let self = self else { return }
      switch state {
      case .ready:
        if settled { return }
        settled = true
        self.conn = c
        self.receive(c)
        done(nil)
      case .failed, .cancelled:
        if settled { return }
        settled = true
        c.cancel()
        if Date() < deadline {
          // The server binds a moment after it starts -- it bakes the nav
          // graph and runs the map's Lua first -- so retry rather than sleep.
          self.queue.asyncAfter(deadline: .now() + 0.05) { self.attempt(port: port, deadline: deadline, server: server, done: done) }
        } else {
          done(NSError(domain: TAG, code: 2, userInfo: [NSLocalizedDescriptionKey: "no link socket on 127.0.0.1:\(port)"]))
        }
      default: break
      }
    }
    c.start(queue: queue)
  }

  private func receive(_ c: NWConnection) {
    c.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
      guard let self = self else { return }
      if let d = data, !d.isEmpty { self.onBytes?(d) }
      if isComplete || error != nil {
        if !self.closed { self.closed = true; self.onClosed?("the game server stopped") }
        return
      }
      self.receive(c)
    }
  }

  func send(_ data: Data) {
    conn?.send(content: data, completion: .contentProcessed { _ in })
  }

  func close() {
    closed = true
    conn?.cancel()
    conn = nil
  }
}

// MARK: - serving the client out of the bundle

final class AssetHandler: NSObject, WKURLSchemeHandler {
  static let types: [String: String] = [
    "html": "text/html", "js": "text/javascript", "mjs": "text/javascript", "wasm": "application/wasm",
    "json": "application/json", "css": "text/css", "png": "image/png", "jpg": "image/jpeg",
    "dnpm": "application/octet-stream", "txt": "text/plain", "lua": "text/plain", "mstk": "application/octet-stream"
  ]
  let web: URL
  let tree: GameTree
  var studio = false
  var bundledModule = ""

  init(tree: GameTree) {
    self.web = Bundle.main.resourceURL!.appendingPathComponent("web")
    self.tree = tree
  }

  func webView(_ webView: WKWebView, start task: WKURLSchemeTask) {
    guard let url = task.request.url, url.absoluteString.hasPrefix(ORIGIN + "/") else { notFound(task); return }
    var path = String(url.absoluteString.dropFirst(ORIGIN.count + 1))
    if let q = path.firstIndex(of: "?") { path = String(path[..<q]) }
    if path.isEmpty { path = "index.html" }
    if path.hasSuffix("/") { path += "index.html" }
    if path.contains("..") { notFound(task); return }
    let ext = (path as NSString).pathExtension.lowercased()
    let type = AssetHandler.types[ext] ?? "application/octet-stream"

    if studio, let data = fromTree(path) { respond(task, url: url, type: type, data: data); return }
    let file = web.appendingPathComponent(path)
    guard let data = try? Data(contentsOf: file) else { notFound(task); return }
    respond(task, url: url, type: type, data: data)
  }

  func webView(_ webView: WKWebView, stop task: WKURLSchemeTask) {}

  private func respond(_ task: WKURLSchemeTask, url: URL, type: String, data: Data) {
    let res = HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1",
                              headerFields: ["Content-Type": type, "Content-Length": String(data.count), "Access-Control-Allow-Origin": "*"])!
    task.didReceive(res)
    task.didReceive(data)
    task.didFinish()
  }

  private func notFound(_ task: WKURLSchemeTask) {
    guard let url = task.request.url else { return }
    task.didReceive(HTTPURLResponse(url: url, statusCode: 404, httpVersion: "HTTP/1.1", headerFields: [:])!)
    task.didFinish()
  }

  /// What the studio's modules add to the bundle's web tree: their art first,
  /// the two catalogues concatenated, the texture index merged. MainActivity's
  /// serveFromTree, the same rules.
  private func fromTree(_ path: String) -> Data? {
    let mods = tree.userModules(except: bundledModule)
    if mods.isEmpty { return nil }
    if path == "props.txt" || path == "animations.txt" {
      var out = (try? Data(contentsOf: web.appendingPathComponent(path))) ?? Data()
      for m in mods {
        if let d = try? Data(contentsOf: m.appendingPathComponent("assets/" + path)) {
          out.append(("\n# --- " + m.lastPathComponent + " ---\n").data(using: .utf8)!)
          out.append(d)
        }
      }
      return out
    }
    if path == "textures/index.json" {
      var sizes: [String: Any] = [:]
      var cutout: [String: Any] = [:]
      var sources = mods.compactMap { try? Data(contentsOf: $0.appendingPathComponent("assets/textures/index.json")) }
      if let base = try? Data(contentsOf: web.appendingPathComponent(path)) { sources.append(base) }
      for src in sources {
        guard let j = (try? JSONSerialization.jsonObject(with: src)) as? [String: Any] else { continue }
        for (k, v) in (j["sizes"] as? [String: Any]) ?? [:] where sizes[k] == nil { sizes[k] = v }
        for (k, v) in (j["cutout"] as? [String: Any]) ?? [:] where cutout[k] == nil { cutout[k] = v }
      }
      return try? JSONSerialization.data(withJSONObject: ["sizes": sizes, "cutout": cutout])
    }
    if path.hasPrefix("textures/") || path.hasPrefix("sprites/") || path.hasPrefix("music/") {
      for m in mods {
        if let d = try? Data(contentsOf: m.appendingPathComponent("assets/" + path)) { return d }
      }
    }
    return nil
  }
}

// MARK: - the controller: the web view, the bridge, the studio's calls

final class ShellController: UIViewController, WKScriptMessageHandlerWithReply, UIDocumentPickerDelegate {
  let tree = GameTree()
  let server = ServerHost()
  let link = Link()
  var web: WKWebView!
  var assets: AssetHandler!
  var bundledModule = ""
  var studio = false
  var pendingImport: ((Any?, String?) -> Void)?

  override func viewDidLoad() {
    super.viewDidLoad()
    view.backgroundColor = UIColor(red: 0.043, green: 0.051, blue: 0.071, alpha: 1)
    assets = AssetHandler(tree: tree)
    let cfg = WKWebViewConfiguration()
    cfg.setURLSchemeHandler(assets, forURLScheme: "dnp")
    cfg.allowsInlineMediaPlayback = true
    cfg.mediaTypesRequiringUserActionForPlayback = []
    cfg.userContentController.addScriptMessageHandler(self, contentWorld: .page, name: "dnp")
    // ios/web/native.js, before any page script: window.dnpNative for the
    // shared shell.js.
    if let src = Bundle.main.resourceURL.flatMap({ try? String(contentsOf: $0.appendingPathComponent("native.js")) }) {
      cfg.userContentController.addUserScript(WKUserScript(source: src, injectionTime: .atDocumentStart, forMainFrameOnly: true))
    }
    web = WKWebView(frame: view.bounds, configuration: cfg)
    web.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    web.scrollView.isScrollEnabled = false
    web.scrollView.bounces = false
    web.isOpaque = false
    web.backgroundColor = view.backgroundColor
    if #available(iOS 16.4, *) { web.isInspectable = true }
    view.addSubview(web)
    DispatchQueue.global(qos: .userInitiated).async { self.boot() }
  }

  override var prefersStatusBarHidden: Bool { true }
  override var prefersHomeIndicatorAutoHidden: Bool { true }

  private func boot() {
    do {
      try tree.seed()
      let start = try tree.start()
      guard let module = start["module"], let map = start["map"] else { throw NSError(domain: TAG, code: 3, userInfo: [NSLocalizedDescriptionKey: "game/start.txt names no module and map; rebuild with scripts/build_ios.sh"]) }
      bundledModule = module
      studio = start["studio"] == "1"
      assets.studio = studio
      assets.bundledModule = module
      let modDir = tree.root.appendingPathComponent("modules/" + module)
      try startServer(modules: [modDir], map: modDir.appendingPathComponent("maps/" + map))
      let page = studio ? "/studio/" : "/index.html?touch=1"
      DispatchQueue.main.async { self.web.load(URLRequest(url: URL(string: ORIGIN + page)!)) }
    } catch {
      log("boot failed: \(error.localizedDescription)")
      let msg = error.localizedDescription.replacingOccurrences(of: "<", with: "&lt;")
      DispatchQueue.main.async {
        self.web.loadHTMLString("<body style='background:#0b0d12;color:#cfe3ff;font:13px monospace;padding:16px'>could not start the game server<br><br>\(msg)</body>", baseURL: URL(string: ORIGIN + "/"))
      }
    }
  }

  /// Start the server and connect the link, blocking the caller's (background) thread.
  private func startServer(modules: [URL], map: URL) throws {
    link.close()
    try server.start(workDir: tree.root, modules: modules, map: map, props: tree.root.appendingPathComponent("assets/props.txt"))
    link.onBytes = { [weak self] d in
      let b64 = d.base64EncodedString()
      DispatchQueue.main.async { self?.web.evaluateJavaScript("window.__dnpDown && window.__dnpDown('\(b64)')") }
    }
    link.onClosed = { [weak self] reason in
      DispatchQueue.main.async { self?.web.evaluateJavaScript("window.__dnpClosed && window.__dnpClosed(\(ShellController.js(reason)))") }
    }
    let sem = DispatchSemaphore(value: 0)
    var failure: Error?
    link.connect(port: server.port, timeout: 20, server: server) { e in failure = e; sem.signal() }
    sem.wait()
    if let e = failure { throw e }
    log("link up")
  }

  static func js(_ s: String) -> String {
    let d = try! JSONSerialization.data(withJSONObject: [s])
    let arr = String(data: d, encoding: .utf8)!
    return String(arr.dropFirst().dropLast())
  }

  // The bridge: `up`, `close`, `log`, and the studio's seven calls.
  func userContentController(_ ucc: WKUserContentController, didReceive message: WKScriptMessage, replyHandler: @escaping (Any?, String?) -> Void) {
    guard let m = message.body as? [String: Any], let op = m["op"] as? String else { replyHandler(nil, "bad message"); return }
    switch op {
    case "up":
      if let s = m["data"] as? String, let d = Data(base64Encoded: s) { link.send(d) }
      replyHandler(nil, nil)
    case "close":
      link.close()
      replyHandler(nil, nil)
    case "log":
      log("[web] \(m["line"] as? String ?? "")")
      replyHandler(nil, nil)
    case "studio":
      let call = m["call"] as? String ?? ""
      let raw = (m["args"] as? String ?? "{}").data(using: .utf8) ?? Data()
      let args = ((try? JSONSerialization.jsonObject(with: raw)) as? [String: Any]) ?? [:]
      DispatchQueue.global(qos: .userInitiated).async { self.studioCall(call, args, replyHandler) }
    default:
      replyHandler(nil, "unknown op \(op)")
    }
  }

  private func studioCall(_ call: String, _ a: [String: Any], _ reply: @escaping (Any?, String?) -> Void) {
    let done: (Any?) -> Void = { r in DispatchQueue.main.async { reply(r, nil) } }
    let fail: (String) -> Void = { e in DispatchQueue.main.async { reply(nil, e) } }
    do {
      switch call {
      case "writeFile":
        guard let f = tree.file(a["path"] as? String ?? ""), let d = Data(base64Encoded: a["data"] as? String ?? "") else { fail("refused path"); return }
        try FileManager.default.createDirectory(at: f.deletingLastPathComponent(), withIntermediateDirectories: true)
        try d.write(to: f)
        done(true)
      case "readFile":
        guard let f = tree.file(a["path"] as? String ?? "") else { fail("refused path"); return }
        done((try? Data(contentsOf: f))?.base64EncodedString())
      case "listFiles":
        let prefix = (a["prefix"] as? String ?? "").replacingOccurrences(of: "/$", with: "", options: .regularExpression)
        let base = prefix.isEmpty ? tree.root : (tree.file(prefix) ?? tree.root)
        var out: [String] = []
        if let e = FileManager.default.enumerator(at: base, includingPropertiesForKeys: [.isDirectoryKey]) {
          for case let u as URL in e where (try? u.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == false {
            let rel = String(u.path.dropFirst(tree.root.path.count + 1))
            out.append(rel)
          }
        }
        done(out.sorted())
      case "deleteFile":
        guard let f = tree.file(a["path"] as? String ?? "") else { fail("refused path"); return }
        done((try? FileManager.default.removeItem(at: f)) != nil)
      case "launch":
        guard let module = a["module"] as? String, let map = a["map"] as? String,
              let modDir = tree.file(module), let mapFile = tree.file(module + "/maps/" + map),
              FileManager.default.fileExists(atPath: mapFile.path) else { fail("no such level"); return }
        let bundled = tree.root.appendingPathComponent("modules/" + bundledModule)
        var mods = [bundled]
        if modDir.standardizedFileURL != bundled.standardizedFileURL { mods.append(modDir) }
        try startServer(modules: mods, map: mapFile)
        DispatchQueue.main.async { self.web.load(URLRequest(url: URL(string: ORIGIN + "/index.html?touch=1&studio=1")!)) }
        done(true)
      case "share":
        guard let d = Data(base64Encoded: a["data"] as? String ?? "") else { fail("bad data"); return }
        let name = a["name"] as? String ?? "pack.zip"
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent(name)
        try d.write(to: tmp)
        DispatchQueue.main.async {
          let sheet = UIActivityViewController(activityItems: [tmp], applicationActivities: nil)
          sheet.completionWithItemsHandler = { _, completed, _, _ in reply(completed, nil) }
          sheet.popoverPresentationController?.sourceView = self.view
          self.present(sheet, animated: true)
        }
      case "importFile":
        DispatchQueue.main.async {
          self.pendingImport = reply
          let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.item], asCopy: true)
          picker.delegate = self
          self.present(picker, animated: true)
        }
      default:
        fail("unknown studio call \(call)")
      }
    } catch {
      fail(error.localizedDescription)
    }
  }

  func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
    let reply = pendingImport
    pendingImport = nil
    guard let u = urls.first, let d = try? Data(contentsOf: u) else { reply?(nil, nil); return }
    reply?(["name": u.lastPathComponent, "data": d.base64EncodedString()], nil)
  }

  func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
    pendingImport?(nil, nil)
    pendingImport = nil
  }
}
