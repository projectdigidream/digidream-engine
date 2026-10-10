package com.digidream.app;

import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.List;

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
 *
 * <p>STARTED MORE THAN ONCE. The studio's Play stops the server and starts it
 * again on the level just written, so {@link #stop()} leaves this in a state
 * {@link #start} accepts, and {@link #connect} begins a fresh reader.
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
   * Start the server on {@code port}, serving {@code modules} and {@code map}.
   *
   * <p>Several modules, in order, exactly as the desktop shell passes them:
   * catalogues are concatenated and a collision refuses to start, which is the
   * server's check to make and not this class's.
   *
   * <p>Its output is forwarded to logcat under the app's tag, unchanged. That
   * is the only diagnostic there is when a level will not load -- the server
   * names the script and the line -- and dropping it would turn every content
   * error into a black screen.
   */
  void start(File exe, File workDir, int port, List<File> modules, File map) throws IOException {
    start(exe, workDir, port, modules, map, null);
  }

  /**
   * As above, and with {@code logFile} every line also written there, the file
   * started afresh: the studio's Play log (logs/play-server.log), which the
   * studio reads back -- on a phone with no logcat, the only place a person
   * can see why a level would not load.
   */
  void start(File exe, File workDir, int port, List<File> modules, File map, File logFile) throws IOException {
    start(exe, workDir, port, modules, java.util.Collections.singletonList(map), logFile);
  }

  /**
   * As above with several levels: the first is where players start, the rest
   * are what a script may send them to (change_map). A play-test passes the
   * map being tested and then every other map of its module -- with only the
   * first declared, a graph going to another map is refused as naming one the
   * server does not have.
   */
  void start(File exe, File workDir, int port, List<File> modules, List<File> maps, File logFile) throws IOException {
    List<String> cmd = new java.util.ArrayList<>();
    cmd.add(exe.getAbsolutePath());
    cmd.add("--port"); cmd.add(String.valueOf(port));
    for (File m : modules) { cmd.add("--module"); cmd.add(m.getAbsolutePath()); }
    for (File m : maps) { cmd.add("--map"); cmd.add(m.getAbsolutePath()); }
    cmd.add("--spawn"); cmd.add("start");
    ProcessBuilder pb = new ProcessBuilder(cmd);
    pb.directory(workDir);
    pb.redirectErrorStream(true);
    stopped = false;
    java.io.OutputStream file = null;
    if (logFile != null) {
      File dir = logFile.getParentFile();
      if (dir != null) dir.mkdirs();
      file = new java.io.FileOutputStream(logFile, false);
      file.write(("$ " + String.join(" ", cmd) + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8));
      file.flush();
    }
    synchronized (tail) { tail.clear(); }
    process = pb.start();

    final java.io.OutputStream sink = file;
    Thread log = new Thread(() -> {
      try (InputStream is = process.getInputStream()) {
        byte[] buf = new byte[4096];
        StringBuilder line = new StringBuilder();
        int n;
        while ((n = is.read(buf)) > 0) {
          for (int i = 0; i < n; i++) {
            char c = (char) (buf[i] & 0xff);
            if (c == '\n') {
              Log.i(TAG, line.toString());
              remember(line.toString());
              if (sink != null) {
                try { sink.write((line + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8)); sink.flush(); } catch (IOException ignored) { }
              }
              line.setLength(0);
            } else line.append(c);
          }
        }
      } catch (IOException ignored) {
      }
      if (sink != null) {
        try {
          String end = process.isAlive() ? "" : "[shell] the server exited (" + process.exitValue() + ")\n";
          sink.write(end.getBytes(java.nio.charset.StandardCharsets.UTF_8));
          sink.close();
        } catch (Exception ignored) { }
      }
    }, "dnp-server-log");
    log.setDaemon(true);
    log.start();
    logThread = log;
  }

  // THE SERVER'S LAST WORDS, kept in memory: a phone has no console and a game
  // outside the studio has no Play log, so when the server dies starting, what
  // it said is put on the screen (see connect).
  private static final int TAIL_LINES = 40;
  private final java.util.ArrayDeque<String> tail = new java.util.ArrayDeque<>();
  private Thread logThread = null;

  private void remember(String line) {
    synchronized (tail) {
      tail.addLast(line);
      while (tail.size() > TAIL_LINES) tail.removeFirst();
    }
  }

  /**
   * What the server said before it died: the lines that say why first (a
   * refusal, a failure, an error), then the end of its output for context.
   */
  String lastWords() {
    // Its output may still be in the pipe when the exit is noticed.
    if (logThread != null) { try { logThread.join(1000); } catch (InterruptedException ignored) { } }
    java.util.List<String> lines;
    synchronized (tail) { lines = new java.util.ArrayList<>(tail); }
    StringBuilder why = new StringBuilder();
    for (String l : lines) {
      String low = l.toLowerCase(java.util.Locale.ROOT);
      if (low.contains("refus") || low.contains("fail") || low.contains("error") || low.contains("cannot")
          || low.contains("could not") || low.contains("problem")) why.append(l).append('\n');
    }
    StringBuilder out = new StringBuilder();
    if (why.length() > 0) out.append(why).append('\n');
    int from = Math.max(0, lines.size() - 15);
    for (int i = from; i < lines.size(); i++) out.append(lines.get(i)).append('\n');
    return out.toString().trim();
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
      if (process != null && !process.isAlive()) {
        String said = lastWords();
        throw new IOException("the game server exited while starting (exit " + process.exitValue() + ")"
            + (said.isEmpty() ? " and said nothing" : ". It said:\n\n" + said));
      }
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

    final Socket mine = socket;
    reader = new Thread(() -> {
      byte[] buf = new byte[64 * 1024];
      try (InputStream is = mine.getInputStream()) {
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

  synchronized void stop() {
    stopped = true;
    try { if (socket != null) socket.close(); } catch (IOException ignored) { }
    socket = null;
    out = null;
    // DESTROYED, not asked. The server has no shutdown message on the link --
    // an edge disappearing is a normal thing for it -- and an orphaned copy
    // holding the port is why the game would not start a second time.
    if (process != null) {
      process.destroy();
      try { process.waitFor(); } catch (InterruptedException ignored) { }
      process = null;
    }
  }
}
