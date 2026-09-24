package com.digidream.app;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.os.Build;
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

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URLConnection;
import java.util.Arrays;
import java.util.HashMap;
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
        return serve(req.getUrl().toString());
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

      File exe = new File(getApplicationInfo().nativeLibraryDir, "libdnpserver.so");
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
      Props start = Props.read(new File(gameDir, "start.txt"));
      File module = new File(gameDir, "modules/" + start.need("module"));
      File map = new File(module, "maps/" + start.need("map"));
      if (!map.exists()) throw new IOException("no such level: " + map);
      int port = GameServer.freePort();
      Log.i(TAG, "starting server on 127.0.0.1:" + port);
      server.start(exe, gameDir, port, module, map);
      server.connect(port, 20000, new GameServer.Sink() {
        @Override public void onBytes(byte[] data, int length) {
          down(Base64.encodeToString(Arrays.copyOf(data, length), Base64.NO_WRAP));
        }
        @Override public void onClosed(String reason) {
          ui.post(() -> web.evaluateJavascript(
              "window.__dnpClosed && window.__dnpClosed(" + quote(reason) + ")", null));
        }
      });

      // ONLY NOW is the page loaded. The client decides its transport at
      // module scope and sends its hello immediately, so a page that started
      // before the link was up would ask an empty socket to let it in.
      ui.post(() -> web.loadUrl(ORIGIN + "/index.html?touch=1"));
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

  /** Bytes from the server, handed to the page as base64. */
  private void down(String b64) {
    ui.post(() -> web.evaluateJavascript(
        "window.__dnpDown && window.__dnpDown('" + b64 + "')", null));
  }

  /** What the page can call. Four methods, all of them a byte pipe. */
  private final class Bridge {
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
    if (path.isEmpty()) path = "index.html";

    // NO `..`, EVER. This serves out of the APK rather than out of a world of
    // files, so there is not much to reach -- but a path check that only holds
    // because of what happens to be next door is the kind that stops holding.
    if (path.contains("..")) return null;

    String ext = path.substring(path.lastIndexOf('.') + 1).toLowerCase();
    String type = TYPES.get(ext);
    if (type == null) type = URLConnection.guessContentTypeFromName(path);
    if (type == null) type = "application/octet-stream";

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

  private static WebResourceResponse notFound() {
    return new WebResourceResponse("text/plain", "utf-8", 404, "Not Found",
        new HashMap<>(), new java.io.ByteArrayInputStream(new byte[0]));
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
