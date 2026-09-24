package com.digidream.app;

import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;

/**
 * The authoritative server, running inside this app, and a byte pipe to it.
 *
 * <p>THE SAME BINARY THE DESKTOP RUNS, cross-compiled for arm64. Not a port,
 * not a reimplementation: {@code scripts/build_apk.sh} feeds the same source
 * list {@code scripts/build_server.sh} does to the NDK's clang. A second
 * implementation of the simulation would be the end of the property the whole
 * engine rests on -- that the client predicts by running the same physics the
 * server will run.
 *
 * <p>WHY IT IS CALLED libdnpserver.so WHEN IT IS AN EXECUTABLE. Android will
 * only exec a file out of the app's own nativeLibraryDir, and only files
 * matching {@code lib*.so} are extracted there from the APK. The name is a
 * packaging requirement and says nothing about what the file is: readelf calls
 * it a PIE executable, which is what it is.
 *
 * <p>THIS CLASS KNOWS NOTHING ABOUT THE GAME. It starts a process, opens a
 * socket and moves bytes. The link framing -- which byte is a session id,
 * which is a length -- stays in JavaScript, in {@code
 * server/edge/src/framing.js}, shared with the edge and the desktop relay. A
 * Java copy of it would be a second implementation of a wire format, which is
 * the one thing a wire format must never have.
 */
final class GameServer {
  private static final String TAG = "digidream";

  /** Bytes arriving from the server, as they arrive. Never whole frames. */
  interface Sink {
    void onBytes(byte[] data, int length);
    void onClosed(String reason);
  }

  private Process process;
  private Socket socket;
  private OutputStream out;
  private Thread reader;
  private volatile boolean stopped;

  /**
   * A port nothing else is using.
   *
   * <p>Asked of the OS rather than picked: a fixed port is a game that fails
   * to start the second time you open it, because the previous process has
   * not finished dying. There is a race between closing this and the server
   * binding it, and on a phone with one app doing this it is not a real one.
   */
  static int freePort() throws IOException {
    try (ServerSocket probe = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"))) {
      return probe.getLocalPort();
    }
  }

  /**
   * Start the server on {@code port}, serving {@code module} and {@code map}.
   *
   * <p>Its output is forwarded to logcat under the app's tag, unchanged. That
   * is the only diagnostic there is when a level will not load -- the server
   * names the script and the line -- and dropping it would turn every content
   * error into a black screen.
   */
  void start(File exe, File workDir, int port, File module, File map) throws IOException {
    ProcessBuilder pb = new ProcessBuilder(
        exe.getAbsolutePath(),
        "--port", String.valueOf(port),
        "--module", module.getAbsolutePath(),
        "--map", map.getAbsolutePath(),
        "--spawn", "start");
    pb.directory(workDir);
    pb.redirectErrorStream(true);
    process = pb.start();

    Thread log = new Thread(() -> {
      try (InputStream is = process.getInputStream()) {
        byte[] buf = new byte[4096];
        StringBuilder line = new StringBuilder();
        int n;
        while ((n = is.read(buf)) > 0) {
          for (int i = 0; i < n; i++) {
            char c = (char) (buf[i] & 0xff);
            if (c == '\n') { Log.i(TAG, line.toString()); line.setLength(0); }
            else line.append(c);
          }
        }
      } catch (IOException ignored) {
      }
    }, "dnp-server-log");
    log.setDaemon(true);
    log.start();
  }

  /**
   * Connect to the server's link socket, retrying while it comes up.
   *
   * <p>The server binds its port a moment after it is spawned -- it loads the
   * map, bakes the nav graph and runs the map's Lua first -- so the first
   * connect always fails. Retrying beats sleeping: a big level takes longer on
   * a phone than on a laptop and a sleep long enough for the worst case is a
   * second of black screen in the common one.
   */
  void connect(int port, int timeoutMs, Sink sink) throws IOException {
    long deadline = System.currentTimeMillis() + timeoutMs;
    IOException last = null;
    while (System.currentTimeMillis() < deadline) {
      try {
        socket = new Socket(InetAddress.getByName("127.0.0.1"), port);
        // The input bundles are tens of bytes at 60 Hz and every one of them
        // is wanted immediately. Nagle would hold them back looking for a
        // bigger packet, on a loopback connection where there is no packet
        // worth saving.
        socket.setTcpNoDelay(true);
        out = socket.getOutputStream();
        break;
      } catch (IOException e) {
        last = e;
        try { Thread.sleep(50); } catch (InterruptedException ignored) { }
      }
    }
    if (socket == null) throw last != null ? last : new IOException("no link socket");

    reader = new Thread(() -> {
      byte[] buf = new byte[64 * 1024];
      try (InputStream is = socket.getInputStream()) {
        int n;
        while (!stopped && (n = is.read(buf)) > 0) sink.onBytes(buf, n);
        if (!stopped) sink.onClosed("the game server stopped");
      } catch (IOException e) {
        if (!stopped) sink.onClosed("the link to the game server failed");
      }
    }, "dnp-link");
    reader.setDaemon(true);
    reader.start();
  }

  /** Raw bytes upstream. Whole frames, but this does not know or care. */
  synchronized void write(byte[] data) {
    if (out == null) return;
    try {
      out.write(data);
      out.flush();
    } catch (IOException e) {
      Log.w(TAG, "link write failed: " + e.getMessage());
    }
  }

  void stop() {
    stopped = true;
    try { if (socket != null) socket.close(); } catch (IOException ignored) { }
    // DESTROYED, not asked. The server has no shutdown message on the link --
    // an edge disappearing is a normal thing for it -- and an orphaned copy
    // holding the port is why the game would not start a second time.
    if (process != null) process.destroy();
  }
}
