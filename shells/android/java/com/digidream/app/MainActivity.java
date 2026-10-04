package com.digidream.app;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.OpenableColumns;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Base64;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URLConnection;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The whole app: a WebView with the client in it, and the server beside it.
 *
 * <p>THE SAME CLIENT THE BROWSER LOADS. Not a port and not a subset -- {@code
 * scripts/build_apk.sh} stages {@code client/} into the assets unchanged, and
 * the one file that differs is {@code shell.js}, which the page declares as a
 * hook precisely so that a shell can fill it in. The desktop build works the
 * same way through Electron's preload. Three shells, one client.
 *
 * <p>WHY THE PAGE IS SERVED FROM AN https:// ORIGIN. WebAssembly, ES modules
 * and a dozen other things the client uses are restricted to a SECURE CONTEXT,
 * and {@code file://} is not one. So the assets are served through {@link
 * #shouldInterceptRequest} on {@code appassets.androidplatform.net} -- a
 * reserved name that cannot resolve, so nothing ever leaves the device -- and
 * the browser treats the result as https, which it is.
 *
 * <p>NOTHING LEAVES THE DEVICE, and it is enforced rather than promised. The
 * app does hold android.permission.INTERNET, because Android gates the
 * creation of ANY socket on it -- 127.0.0.1 included, which is the only socket
 * here. So the guarantee cannot rest on the manifest: it rests on {@link
 * #serve}, which answers every request the page makes and answers anything
 * that is not our own asset origin with a 404 instead of letting WebView fetch
 * it. The server is a child process, the link is loopback, and every asset is
 * in the APK.
 */
public final class MainActivity extends Activity {
  private static final String TAG = "digidream";

  /**
   * The origin the client runs on.
   *
   * <p>The same name AndroidX's WebViewAssetLoader uses, and for its reason:
   * {@code androidplatform.net} is reserved and does not resolve, so a request
   * this class fails to intercept fails closed -- it cannot silently reach
   * some other server. A made-up domain could one day be registered by
   * somebody else.
   */
  private static final String ORIGIN = "https://appassets.androidplatform.net";

  private final Handler ui = new Handler(Looper.getMainLooper());
  private final GameServer server = new GameServer();
  private WebView web;
  private File gameDir;
  private File exe;
  /** The module the build shipped; the studio's own live beside it under gameDir/modules. */
  private String bundledModule;
  /** Whether the build staged the studio (start.txt's `studio=1`): opens it first, serves its modules. */
  private boolean studio;
  /** What the build wrote to start.txt, kept for the missing-page report. */
  private Props start;

  // The two studio calls that need the platform's own picker or share sheet
  // answer through onActivityResult; the request they came from waits here.
  private static final int REQ_SHARE = 1;
  private static final int REQ_IMPORT = 2;
  private String pendingShareId;
  private byte[] pendingShareBytes;
  private String pendingImportId;

  @Override
  @SuppressLint("SetJavaScriptEnabled")
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    immersive();

    // DEBUGGABLE FROM chrome://inspect. The client's own diagnostics -- the
    // HUD's frame timings, the prediction counters, the server's log relayed
    // into the console -- are the only way to tell a rendering problem from a
    // netcode one, and on a phone there is nowhere else for them to go.
    WebView.setWebContentsDebuggingEnabled(true);

    web = new WebView(this);
    WebSettings s = web.getSettings();
    s.setJavaScriptEnabled(true);
    s.setDomStorageEnabled(true);
    s.setMediaPlaybackRequiresUserGesture(false);
    // The page sets its own viewport and lays itself out for the screen it is
    // on. Letting WebView apply its "desktop page on a phone" heuristics would
    // undo that and render the game at a third of the size.
    s.setUseWideViewPort(false);
    s.setLoadWithOverviewMode(false);
    s.setSupportZoom(false);
    s.setBuiltInZoomControls(false);
    web.setBackgroundColor(0xff0b0d12);

    web.setWebViewClient(new WebViewClient() {
      @Override
      public WebResourceResponse shouldInterceptRequest(WebView v, WebResourceRequest req) {
        WebResourceResponse res = serve(req.getUrl().toString());
        // THE PAGE ITSELF IS NEVER A BARE 404. WebView shows one as "web page
        // not available, ERR_HTTP_RESPONSE_CODE_FAILURE", which names neither
        // the page nor the build, and a phone that was handed an APK has no
        // logcat to ask. So a missing page is answered with a page that says
        // what was asked, what the build wrote to start.txt, and what the APK
        // holds: a screenshot of it is a bug report.
        if (req.isForMainFrame() && res != null && res.getStatusCode() == 404) {
          return bytes("text/html", missingPage(req.getUrl().toString()).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        }
        return res;
      }
    });
    // THE GAME PAGE'S CONSOLE, into the studio's Play log while a play-test
    // runs: an uncaught error, a refused map, a failed fetch -- what logcat
    // would show, where the studio can read it back (logs/play-game.log).
    web.setWebChromeClient(new android.webkit.WebChromeClient() {
      @Override
      public boolean onConsoleMessage(android.webkit.ConsoleMessage m) {
        playLog("[" + m.messageLevel().name().toLowerCase(java.util.Locale.ROOT) + "] " + m.message()
            + (m.sourceId() == null || m.sourceId().isEmpty() ? "" : " (" + m.sourceId().replace(ORIGIN, "") + ":" + m.lineNumber() + ")"));
        return false;
      }
    });
    web.addJavascriptInterface(new Bridge(), "dnpNative");
    setContentView(web);

    new Thread(this::boot, "dnp-boot").start();
  }

  /**
   * Unpack, start the server, and open the page.
   *
   * <p>Off the UI thread: unpacking a module is tens of megabytes of file
   * copying and the server takes a moment to bake a level's nav graph. Doing
   * either on the main thread is an ANR on a slow phone, which Android
   * presents to the user as "Digidream isn't responding".
   */
  private void boot() {
    try {
      gameDir = new File(getFilesDir(), "game");
      unpack("game", gameDir);

      exe = new File(getApplicationInfo().nativeLibraryDir, "libdnpserver.so");
      if (!exe.exists()) throw new IOException("no server binary at " + exe);

      // WHICH MODULE AND WHICH LEVEL, from the file the build wrote.
      //
      // Both were constants here, and scripts/build_apk.sh took a --map
      // argument, checked it existed, and then never passed it on: every APK
      // started map01 whatever you asked for. A flag that is documented and
      // ignored is worse than no flag, and this is the fix -- the build states
      // its choice in the assets and this reads it.
      //
      // ALL 32 LEVELS ARE IN THE APK regardless. The staging copies the whole
      // module, which is 2.1 MB of baked geometry for the lot, so this names a
      // starting point rather than deciding what shipped.
      start = Props.read(new File(gameDir, "start.txt"));
      bundledModule = start.need("module");
      studio = "1".equals(start.get("studio"));
      File module = new File(gameDir, "modules/" + bundledModule);
      File map = new File(module, "maps/" + start.need("map"));
      if (!map.exists()) throw new IOException("no such level: " + map);
      List<File> modules = new ArrayList<>();
      modules.add(module);
      startServer(modules, map);

      // ONLY NOW is the page loaded. The client decides its transport at
      // module scope and sends its hello immediately, so a page that started
      // before the link was up would ask an empty socket to let it in.
      //
      // With the studio, the studio comes first; its Play button is what
      // loads the game page (see launch()).
      final String page = studio ? "/studio/" : "/index.html?touch=1";
      ui.post(() -> web.loadUrl(ORIGIN + page));
    } catch (Exception e) {
      Log.e(TAG, "boot failed", e);
      final String msg = String.valueOf(e.getMessage());
      ui.post(() -> {
        web.loadDataWithBaseURL(ORIGIN + "/", "<body style='background:#0b0d12;color:#cfe3ff;"
            + "font:13px monospace;padding:16px'>could not start the game server<br><br>"
            + esc(msg) + "</body>", "text/html", "utf-8", null);
      });
    }
  }

  /** Start the server on a fresh port and connect the link to it. */
  private void startServer(List<File> modules, File map) throws IOException {
    startServer(modules, map, null);
  }

  private void startServer(List<File> modules, File map, File logFile) throws IOException {
    startServer(modules, java.util.Collections.singletonList(map), logFile);
  }

  private void startServer(List<File> modules, List<File> maps, File logFile) throws IOException {
    int port = GameServer.freePort();
    Log.i(TAG, "starting server on 127.0.0.1:" + port + " for " + maps.get(0) + (maps.size() > 1 ? " and " + (maps.size() - 1) + " more" : ""));
    server.start(exe, gameDir, port, modules, maps, logFile);
    server.connect(port, 20000, new GameServer.Sink() {
      @Override public void onBytes(byte[] data, int length) {
        down(Base64.encodeToString(Arrays.copyOf(data, length), Base64.NO_WRAP));
      }
      @Override public void onClosed(String reason) {
        ui.post(() -> web.evaluateJavascript(
            "window.__dnpClosed && window.__dnpClosed(" + quote(reason) + ")", null));
      }
    });
  }

  /** Bytes from the server, handed to the page as base64. */
  private void down(String b64) {
    ui.post(() -> web.evaluateJavascript(
        "window.__dnpDown && window.__dnpDown('" + b64 + "')", null));
  }

  // --- the studio's calls -----------------------------------------------------

  /** A path under the writable game tree, or an exception. Never `..`. */
  private File rwFile(String rel) throws IOException {
    if (rel == null || rel.isEmpty() || rel.startsWith("/") || rel.contains("\\") || rel.contains("\0")) {
      throw new IOException("refused path: " + rel);
    }
    for (String seg : rel.split("/")) {
      if (seg.isEmpty() || seg.equals(".") || seg.equals("..")) throw new IOException("refused path: " + rel);
    }
    return new File(gameDir, rel);
  }

  /** The studio's modules: every directory under gameDir/modules but the bundled one. */
  private List<File> userModules() {
    List<File> out = new ArrayList<>();
    File[] kids = gameDir == null ? null : new File(gameDir, "modules").listFiles();
    if (kids == null) return out;
    Arrays.sort(kids);
    for (File k : kids) if (k.isDirectory() && !k.getName().equals(bundledModule)) out.add(k);
    return out;
  }

  private static byte[] readAllBytes(InputStream in) throws IOException {
    ByteArrayOutputStream bo = new ByteArrayOutputStream();
    byte[] buf = new byte[64 * 1024];
    int n;
    while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
    return bo.toByteArray();
  }

  /** Answer a studio call on the page: window.__dnpStudioReply(id, ok, result). */
  private void reply(String id, boolean ok, String resultJson) {
    final String js = "window.__dnpStudioReply && window.__dnpStudioReply("
        + JSONObject.quote(id) + "," + ok + "," + resultJson + ")";
    ui.post(() -> web.evaluateJavascript(js, null));
  }

  /**
   * One studio call, off the UI thread. The seven names every shell
   * implements (docs/STUDIO.md); bytes travel as base64 because a JavaScript
   * interface carries strings.
   */
  private void studioCall(String id, String op, JSONObject a) {
    try {
      switch (op) {
        case "writeFile": {
          File f = rwFile(a.getString("path"));
          f.getParentFile().mkdirs();
          try (OutputStream os = new FileOutputStream(f)) {
            os.write(Base64.decode(a.getString("data"), Base64.NO_WRAP));
          }
          reply(id, true, "true");
          return;
        }
        case "readFile": {
          File f = rwFile(a.getString("path"));
          if (!f.isFile()) { reply(id, true, "null"); return; }
          try (InputStream in = new FileInputStream(f)) {
            reply(id, true, JSONObject.quote(Base64.encodeToString(readAllBytes(in), Base64.NO_WRAP)));
          }
          return;
        }
        case "listFiles": {
          String prefix = a.optString("prefix", "");
          File base = prefix.isEmpty() ? gameDir : rwFile(prefix);
          JSONArray out = new JSONArray();
          listInto(base, prefix.replaceAll("/$", ""), out);
          reply(id, true, out.toString());
          return;
        }
        case "deleteFile": {
          reply(id, true, String.valueOf(rwFile(a.getString("path")).delete()));
          return;
        }
        case "launch": {
          launch(a.getString("module"), a.getString("map"));
          reply(id, true, "true");
          return;
        }
        case "share": {
          pendingShareId = id;
          pendingShareBytes = Base64.decode(a.getString("data"), Base64.NO_WRAP);
          Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
          i.addCategory(Intent.CATEGORY_OPENABLE);
          i.setType("application/zip");
          i.putExtra(Intent.EXTRA_TITLE, a.getString("name"));
          ui.post(() -> startActivityForResult(i, REQ_SHARE));
          return;
        }
        case "importFile": {
          pendingImportId = id;
          Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
          i.addCategory(Intent.CATEGORY_OPENABLE);
          i.setType("*/*");
          ui.post(() -> startActivityForResult(i, REQ_IMPORT));
          return;
        }
        default:
          reply(id, false, JSONObject.quote("unknown studio call: " + op));
      }
    } catch (Exception e) {
      Log.w(TAG, "studio " + op + " failed", e);
      reply(id, false, JSONObject.quote(String.valueOf(e.getMessage())));
    }
  }

  private static void listInto(File dir, String rel, JSONArray out) {
    File[] kids = dir.listFiles();
    if (kids == null) return;
    Arrays.sort(kids);
    for (File k : kids) {
      String r = rel.isEmpty() ? k.getName() : rel + "/" + k.getName();
      if (k.isDirectory()) listInto(k, r, out); else out.put(r);
    }
  }

  /**
   * The studio's Play: stop the server, start it on the module just written,
   * and show the game with a way back.
   *
   * <p>The bundled module comes along for its catalogue and art; the studio's
   * module comes after it, exactly as the desktop shell orders them.
   */
  private void launch(String module, String map) throws IOException {
    File modDir = rwFile(module);
    File mapFile = new File(modDir, "maps/" + map);
    if (!mapFile.isFile()) throw new IOException("no such level: " + module + "/maps/" + map);
    server.stop();
    List<File> modules = new ArrayList<>();
    File bundled = new File(gameDir, "modules/" + bundledModule);
    modules.add(bundled);
    if (!modDir.getCanonicalPath().equals(bundled.getCanonicalPath())) modules.add(modDir);
    // The Play log, started afresh: the server's output (play-server.log) and
    // the game page's console (play-game.log), for the studio to read back.
    File logs = rwFile("logs");
    logs.mkdirs();
    synchronized (this) {
      gameLog = new File(logs, "play-game.log");
      try (OutputStream os = new java.io.FileOutputStream(gameLog, false)) {
        os.write(("[shell] play-test of " + module + "/maps/" + map + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
      }
    }
    // The map being tested first -- where players start -- then every other
    // map of the module, so a graph's change_map finds the one it names.
    List<File> maps = new ArrayList<>();
    maps.add(mapFile);
    File[] others = new File(modDir, "maps").listFiles((d, n) -> n.endsWith(".dnpm"));
    if (others != null) {
      Arrays.sort(others);
      for (File f : others) if (!f.equals(mapFile) && maps.size() < 64) maps.add(f);
    }
    try {
      startServer(modules, maps, new File(logs, "play-server.log"));
    } catch (IOException e) {
      playLog("[shell] the game did not start: " + e.getMessage());
      throw e;
    }
    ui.post(() -> web.loadUrl(ORIGIN + "/index.html?touch=1&studio=1"));
  }

  /** The game page's log for the studio, while a play-test runs. */
  private File gameLog = null;

  private synchronized void playLog(String line) {
    if (gameLog == null) return;
    try (OutputStream os = new java.io.FileOutputStream(gameLog, true)) {
      os.write((line + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
    } catch (IOException ignored) { }
  }

  @Override
  protected void onActivityResult(int req, int result, Intent data) {
    super.onActivityResult(req, result, data);
    Uri uri = result == RESULT_OK && data != null ? data.getData() : null;
    if (req == REQ_SHARE) {
      final String id = pendingShareId;
      final byte[] bytes = pendingShareBytes;
      pendingShareId = null; pendingShareBytes = null;
      if (id == null) return;
      if (uri == null) { reply(id, true, "false"); return; }
      new Thread(() -> {
        try (OutputStream os = getContentResolver().openOutputStream(uri)) {
          os.write(bytes);
          reply(id, true, "true");
        } catch (Exception e) {
          reply(id, false, JSONObject.quote(String.valueOf(e.getMessage())));
        }
      }, "dnp-share").start();
    } else if (req == REQ_IMPORT) {
      final String id = pendingImportId;
      pendingImportId = null;
      if (id == null) return;
      if (uri == null) { reply(id, true, "null"); return; }
      new Thread(() -> {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
          String name = "file";
          try (Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
              int col = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
              if (col >= 0) name = c.getString(col);
            }
          }
          JSONObject o = new JSONObject();
          o.put("name", name);
          o.put("data", Base64.encodeToString(readAllBytes(in), Base64.NO_WRAP));
          reply(id, true, o.toString());
        } catch (Exception e) {
          reply(id, false, JSONObject.quote(String.valueOf(e.getMessage())));
        }
      }, "dnp-import").start();
    }
  }

  /** What the page can call: a byte pipe, and the studio's calls. */
  private final class Bridge {
    /**
     * A studio call. Answered later through window.__dnpStudioReply(id, ok,
     * result), because two of the seven open a system dialog and none of them
     * should run on the WebView's thread.
     */
    @JavascriptInterface
    public void studio(String id, String op, String argsJson) {
      new Thread(() -> {
        JSONObject a;
        try { a = new JSONObject(argsJson == null || argsJson.isEmpty() ? "{}" : argsJson); }
        catch (Exception e) { reply(id, false, JSONObject.quote("bad arguments")); return; }
        studioCall(id, op, a);
      }, "dnp-studio").start();
    }

    /** Raw bytes upstream, base64 because a JS interface carries strings. */
    @JavascriptInterface
    public void up(String b64) {
      server.write(Base64.decode(b64, Base64.NO_WRAP));
    }

    /** The page is done with the link. */
    @JavascriptInterface
    public void close() {
      server.stop();
    }

    /** So the shell's JS can log where a person will see it. */
    @JavascriptInterface
    public void log(String line) {
      Log.i(TAG, "[web] " + line);
      playLog("[web] " + line);
    }
  }

  // --- serving the client out of the APK ------------------------------------

  private static final Map<String, String> TYPES = new HashMap<>();
  static {
    // WebAssembly and ES modules are both refused on a wrong content type --
    // a .wasm served as octet-stream fails instantiateStreaming, and a module
    // served as text/plain is not executed at all. URLConnection's guesser
    // knows neither, so the ones that matter are named here.
    TYPES.put("html", "text/html");
    TYPES.put("js", "text/javascript");
    TYPES.put("mjs", "text/javascript");
    TYPES.put("wasm", "application/wasm");
    TYPES.put("json", "application/json");
    TYPES.put("css", "text/css");
    TYPES.put("png", "image/png");
    TYPES.put("jpg", "image/jpeg");
    TYPES.put("ogg", "audio/ogg");
    TYPES.put("mp3", "audio/mpeg");
    TYPES.put("wav", "audio/wav");
    TYPES.put("dnpm", "application/octet-stream");
    TYPES.put("txt", "text/plain");
  }

  private WebResourceResponse serve(String url) {
    // ANYTHING THAT IS NOT OURS IS REFUSED, not passed through.
    //
    // Returning null here lets WebView go and fetch the URL, and the app holds
    // the INTERNET permission because a loopback socket needs one -- so "the
    // game never contacts anything" would be a promise rather than a fact.
    // This is the fact: every request is answered here, and one that is not
    // for our own assets is answered with a 404.
    // A SCHEME THAT CANNOT REACH THE NETWORK IS NOT OURS TO REFUSE. `data:`,
    // `blob:` and `about:` are all served out of the page's own memory, and
    // the boot-failure screen is a loadDataWithBaseURL -- so refusing them
    // meant the one page that explains why the game did not start was itself
    // blocked, which the log showed and the screen did not.
    if (url.startsWith("data:") || url.startsWith("blob:") || url.startsWith("about:")) {
      return null;
    }
    if (!url.startsWith(ORIGIN + "/")) {
      Log.w(TAG, "refused an off-origin request: " + url);
      return notFound();
    }
    String path = url.substring(ORIGIN.length() + 1);
    int q = path.indexOf('?');
    if (q >= 0) path = path.substring(0, q);
    // A directory is its index.html, as tools/devserver.js and app/static.js
    // have it: /studio/ is the studio's page. Without this the WebView asked
    // the APK for a directory, got a 404, and showed it as a page error.
    if (path.isEmpty() || path.endsWith("/")) path += "index.html";

    // NO `..`, EVER. This serves out of the APK rather than out of a world of
    // files, so there is not much to reach -- but a path check that only holds
    // because of what happens to be next door is the kind that stops holding.
    if (path.contains("..")) return null;

    String ext = path.substring(path.lastIndexOf('.') + 1).toLowerCase();
    String type = TYPES.get(ext);
    if (type == null) type = URLConnection.guessContentTypeFromName(path);
    if (type == null) type = "application/octet-stream";

    // THE STUDIO'S MODULES, from the writable tree, first -- the same
    // per-file rule as everywhere else, with the two catalogues concatenated
    // and the texture index merged (app/static.js does the same).
    if (studio && gameDir != null) {
      try {
        WebResourceResponse fromTree = serveFromTree(path, type);
        if (fromTree != null) return fromTree;
      } catch (IOException e) {
        Log.w(TAG, "serving " + path + " from the game tree failed: " + e.getMessage());
      }
    }

    try {
      InputStream in = getAssets().open("web/" + path);
      WebResourceResponse res = new WebResourceResponse(type, "utf-8", in);
      Map<String, String> headers = new HashMap<>();
      // The client fetches its own sources and its module's assets from this
      // origin; same-origin needs no CORS header, but the wasm module is
      // fetched with a Request that WebView treats as cross-origin on some
      // builds, and one permissive header here is cheaper than finding out
      // which ones on somebody's phone.
      headers.put("Access-Control-Allow-Origin", "*");
      res.setResponseHeaders(headers);
      return res;
    } catch (IOException e) {
      // 404 EXPLICITLY, rather than null. Returning null lets WebView try the
      // network for a name that cannot resolve, and the failure surfaces as a
      // DNS error that reads like the device is offline.
      return notFound();
    }
  }

  /** The page shown instead of a 404 on the main frame: what was asked, what the build said, what the APK holds. */
  private String missingPage(String url) {
    StringBuilder b = new StringBuilder("<body style='background:#0b0d12;color:#cfe3ff;font:13px monospace;padding:16px'>");
    b.append("<b>Digidream has no page at</b><br>").append(esc(url)).append("<br><br>");
    b.append("start.txt: ").append(start == null ? "(not read yet)" : esc(start.toString())).append("<br><br>");
    for (String dir : new String[] {"web", "web/studio"}) {
      String names;
      try {
        String[] l = getAssets().list(dir);
        names = l == null || l.length == 0 ? "(nothing)" : String.join(" ", l);
      } catch (IOException e) {
        names = String.valueOf(e.getMessage());
      }
      b.append(esc(dir)).append("/: ").append(esc(names)).append("<br>");
    }
    return b.append("</body>").toString();
  }

  private static WebResourceResponse notFound() {
    return new WebResourceResponse("text/plain", "utf-8", 404, "Not Found",
        new HashMap<>(), new java.io.ByteArrayInputStream(new byte[0]));
  }

  private WebResourceResponse bytes(String type, byte[] body) {
    WebResourceResponse res = new WebResourceResponse(type, "utf-8", new ByteArrayInputStream(body));
    Map<String, String> headers = new HashMap<>();
    headers.put("Access-Control-Allow-Origin", "*");
    res.setResponseHeaders(headers);
    return res;
  }

  private byte[] assetBytes(String path) {
    try (InputStream in = getAssets().open("web/" + path)) { return readAllBytes(in); }
    catch (IOException e) { return null; }
  }

  /**
   * What the studio's modules add to what the APK has, or null to fall
   * through to the APK.
   */
  private WebResourceResponse serveFromTree(String path, String type) throws IOException {
    List<File> mods = userModules();
    if (mods.isEmpty()) return null;
    if (path.equals("props.txt") || path.equals("animations.txt")) {
      ByteArrayOutputStream bo = new ByteArrayOutputStream();
      byte[] base = assetBytes(path);
      if (base != null) bo.write(base);
      for (File m : mods) {
        File f = new File(m, "assets/" + path);
        if (!f.isFile()) continue;
        bo.write(("\n# --- " + m.getName() + " ---\n").getBytes("utf-8"));
        try (InputStream in = new FileInputStream(f)) { bo.write(readAllBytes(in)); }
      }
      return bytes("text/plain", bo.toByteArray());
    }
    // The music index likewise: one table for the client, keyed by map hash,
    // the union of every module's (app/static.js does the same).
    if (path.equals("music/index.json")) {
      try {
        JSONObject out = new JSONObject();
        List<byte[]> sources = new ArrayList<>();
        for (File m : mods) {
          File f = new File(m, "assets/music/index.json");
          if (f.isFile()) try (InputStream in = new FileInputStream(f)) { sources.add(readAllBytes(in)); }
        }
        byte[] base = assetBytes(path);
        if (base != null) sources.add(base);
        for (byte[] src : sources) {
          JSONObject j = new JSONObject(new String(src, "utf-8"));
          for (java.util.Iterator<String> it = j.keys(); it.hasNext();) { String k = it.next(); if (!out.has(k)) out.put(k, j.get(k)); }
        }
        return bytes("application/json", out.toString().getBytes("utf-8"));
      } catch (org.json.JSONException e) {
        throw new IOException("bad music index: " + e.getMessage());
      }
    }
    // The UI layout index (docs/UI_LAYOUT.md): the first module that says
    // wins, then the APK's own -- every asset's rule, as tools/devserver.js.
    if (path.equals("ui/index.json")) {
      try {
        JSONObject maps = new JSONObject();
        String dflt = null;
        List<byte[]> sources = new ArrayList<>();
        for (File m : mods) {
          File f = new File(m, "assets/ui/index.json");
          if (f.isFile()) try (InputStream in = new FileInputStream(f)) { sources.add(readAllBytes(in)); }
        }
        byte[] base = assetBytes(path);
        if (base != null) sources.add(base);
        for (byte[] src : sources) {
          JSONObject j = new JSONObject(new String(src, "utf-8"));
          if (dflt == null && j.has("default") && !j.isNull("default")) dflt = j.getString("default");
          JSONObject mp = j.optJSONObject("maps");
          if (mp != null) for (java.util.Iterator<String> it = mp.keys(); it.hasNext();) { String k = it.next(); if (!maps.has(k)) maps.put(k, mp.get(k)); }
        }
        JSONObject out = new JSONObject();
        out.put("version", 1);
        out.put("default", dflt == null ? JSONObject.NULL : dflt);
        out.put("maps", maps);
        return bytes("application/json", out.toString().getBytes("utf-8"));
      } catch (org.json.JSONException e) {
        throw new IOException("bad ui index: " + e.getMessage());
      }
    }
    if (path.equals("textures/index.json")) {
      try {
        JSONObject sizes = new JSONObject();
        JSONObject cutout = new JSONObject();
        List<byte[]> sources = new ArrayList<>();
        for (File m : mods) {
          File f = new File(m, "assets/textures/index.json");
          if (f.isFile()) try (InputStream in = new FileInputStream(f)) { sources.add(readAllBytes(in)); }
        }
        byte[] base = assetBytes(path);
        if (base != null) sources.add(base);
        for (byte[] src : sources) {
          JSONObject j = new JSONObject(new String(src, "utf-8"));
          JSONObject s = j.optJSONObject("sizes");
          JSONObject c = j.optJSONObject("cutout");
          if (s != null) for (java.util.Iterator<String> it = s.keys(); it.hasNext();) { String k = it.next(); if (!sizes.has(k)) sizes.put(k, s.get(k)); }
          if (c != null) for (java.util.Iterator<String> it = c.keys(); it.hasNext();) { String k = it.next(); if (!cutout.has(k)) cutout.put(k, c.get(k)); }
        }
        JSONObject out = new JSONObject();
        out.put("sizes", sizes);
        out.put("cutout", cutout);
        return bytes("application/json", out.toString().getBytes("utf-8"));
      } catch (org.json.JSONException e) {
        throw new IOException("bad texture index: " + e.getMessage());
      }
    }
    if (path.startsWith("textures/") || path.startsWith("sprites/") || path.startsWith("music/") ||
        path.startsWith("sounds/") || path.startsWith("ui/")) {
      for (File m : mods) {
        File f = new File(m, "assets/" + path);
        if (f.isFile()) {
          try (InputStream in = new FileInputStream(f)) { return bytes(type, readAllBytes(in)); }
        }
      }
    }
    return null;
  }

  /**
   * Copy an asset tree onto the filesystem.
   *
   * <p>THE SERVER OPENS FILES BY PATH -- maps, module scripts, the prop
   * catalogue -- and an asset inside an APK is not a file. So the parts it
   * reads are unpacked once; the parts only the WebView reads are served from
   * the APK directly and are never copied.
   *
   * <p>Skipped when the marker matches this build. Re-copying tens of
   * megabytes on every launch is a visible delay, and the marker is the
   * version rather than a flag so that installing a new APK over an old one
   * replaces content that has changed.
   */
  private void unpack(String assetDir, File dest) throws IOException {
    File marker = new File(dest, ".unpacked");
    String want = buildStamp();
    if (marker.exists() && want.equals(readAll(marker))) return;

    copyTree(assetDir, dest);
    try (OutputStream os = new FileOutputStream(marker)) {
      os.write(want.getBytes("utf-8"));
    }
  }

  private String buildStamp() {
    try {
      return String.valueOf(getPackageManager()
          .getPackageInfo(getPackageName(), 0).lastUpdateTime);
    } catch (Exception e) {
      return "0";
    }
  }

  private String readAll(File f) {
    try (InputStream in = new java.io.FileInputStream(f)) {
      byte[] b = new byte[(int) f.length()];
      int n = in.read(b);
      return new String(b, 0, Math.max(n, 0), "utf-8");
    } catch (IOException e) {
      return "";
    }
  }

  private void copyTree(String assetDir, File dest) throws IOException {
    String[] kids = getAssets().list(assetDir);
    if (kids == null || kids.length == 0) {
      // A leaf: AssetManager cannot tell a file from an empty directory, so
      // "no children" means "try to open it as a file" and a failure there
      // means it really was an empty directory.
      dest.getParentFile().mkdirs();
      try (InputStream in = getAssets().open(assetDir);
           OutputStream out = new FileOutputStream(dest)) {
        byte[] buf = new byte[64 * 1024];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
      } catch (IOException e) {
        dest.mkdirs();
      }
      return;
    }
    dest.mkdirs();
    for (String kid : kids) copyTree(assetDir + "/" + kid, new File(dest, kid));
  }

  // --- window ---------------------------------------------------------------

  private void immersive() {
    View d = getWindow().getDecorView();
    d.setSystemUiVisibility(
        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
      // Draw into the notch. The page respects env(safe-area-inset-*) and
      // keeps its text clear of it; letterboxing instead would waste the part
      // of a phone's screen the game most wants.
      getWindow().getAttributes().layoutInDisplayCutoutMode =
          WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
    }
  }

  @Override
  public void onWindowFocusChanged(boolean has) {
    super.onWindowFocusChanged(has);
    if (has) immersive();
  }

  @Override
  protected void onDestroy() {
    server.stop();
    if (web != null) web.destroy();
    super.onDestroy();
  }

  /**
   * The two lines the build wrote: which module, which level.
   *
   * A file rather than a generated Java constant, because a constant means
   * recompiling the shell to change the level and this way the same classes.dex
   * serves every module. Deliberately not a JSON parser: two keys do not need
   * one, and org.json's failure mode on a typo is a null nobody checked.
   */
  private static final class Props {
    private final Map<String, String> kv = new HashMap<>();

    static Props read(File f) throws IOException {
      Props p = new Props();
      try (InputStream in = new java.io.FileInputStream(f)) {
        byte[] b = new byte[(int) f.length()];
        int n = in.read(b);
        for (String line : new String(b, 0, Math.max(n, 0), "utf-8").split("\n")) {
          line = line.trim();
          if (line.isEmpty() || line.startsWith("#")) continue;
          int eq = line.indexOf('=');
          if (eq > 0) p.kv.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
        }
      }
      return p;
    }

    /** Optional: null when the build did not write it. */
    String get(String key) { return kv.get(key); }

    @Override public String toString() { return kv.toString(); }

    /** Refused loudly. A missing key here is a build that did not finish. */
    String need(String key) throws IOException {
      String v = kv.get(key);
      if (v == null || v.isEmpty()) {
        throw new IOException("game/start.txt has no '" + key + "'; rebuild with scripts/build_apk.sh");
      }
      return v;
    }
  }

  private static String esc(String s) {
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
  }

  private static String quote(String s) {
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'";
  }
}
