package com.digidream.app;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * THE GAME'S LOG FILE, when the build asked for one (start.txt `log=1`;
 * build_apk.sh --log, or the packager's "Write a log file").
 *
 * <p>Somewhere the player can open without adb, because the phone is not
 * cabled to anything: Download/Digidream/ (Android 10 and later, through
 * MediaStore, which needs no permission for the app's own files), or the app's
 * external files directory before that (Android/data/&lt;package&gt;/files/logs,
 * which a file manager or a USB connection can reach on those versions).
 *
 * <p>One file per game, &lt;module&gt;-log.txt, started afresh at every launch,
 * with the launch before it kept as &lt;module&gt;-log-previous.txt -- a crash that makes the
 * player restart would otherwise overwrite the log that saw it.
 *
 * <p>Everything goes in: the shell's own lines, the server's output and the
 * game page's console, each line stamped with the time.
 */
final class GameLog {
  private static final String DIR = "Digidream";

  private OutputStream out = null;
  private String where = null;
  private final SimpleDateFormat clock = new SimpleDateFormat("HH:mm:ss.SSS", Locale.ROOT);

  /** Where the log is, for the player: "Download/Digidream/doom2-log.txt". Null when off. */
  String where() { return where; }

  boolean on() { return out != null; }

  /** Start the log for this launch. Failing to is said in logcat and is not fatal. */
  void open(Context ctx, String module, String header) {
    // .txt, which is what a phone opens with a text viewer (and what Android
    // would append to a text/plain file anyway).
    String name = module + "-log.txt";
    String prev = module + "-log-previous.txt";
    try {
      if (Build.VERSION.SDK_INT >= 29) {
        ContentResolver cr = ctx.getContentResolver();
        Uri coll = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
        String rel = android.os.Environment.DIRECTORY_DOWNLOADS + "/" + DIR + "/";
        // The app sees only its own entries here, so these find our files.
        delete(cr, coll, rel, prev);
        Uri last = find(cr, coll, rel, name);
        if (last != null) {
          ContentValues v = new ContentValues();
          v.put(MediaStore.MediaColumns.DISPLAY_NAME, prev);
          cr.update(last, v, null, null);
        }
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
        v.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, rel);
        Uri uri = cr.insert(coll, v);
        if (uri == null) throw new IOException("MediaStore refused the log file");
        out = cr.openOutputStream(uri, "w");
        where = "Download/" + DIR + "/" + name;
      } else {
        File dir = new File(ctx.getExternalFilesDir(null), "logs");
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot make " + dir);
        File f = new File(dir, name);
        File p = new File(dir, prev);
        if (p.exists()) p.delete();
        if (f.exists()) f.renameTo(p);
        out = new FileOutputStream(f, false);
        where = f.getAbsolutePath();
      }
      line("[shell] " + header);
    } catch (Exception e) {
      android.util.Log.w("digidream", "no log file: " + e);
      out = null;
      where = null;
    }
  }

  /** One line, stamped and flushed: a log that stops at a crash must hold what led to it. */
  synchronized void line(String s) {
    if (out == null) return;
    try {
      out.write((clock.format(new Date()) + " " + s + "\n").getBytes(StandardCharsets.UTF_8));
      out.flush();
    } catch (IOException e) {
      out = null;
    }
  }

  private static Uri find(ContentResolver cr, Uri coll, String rel, String name) {
    try (Cursor c = cr.query(coll, new String[] { MediaStore.MediaColumns._ID },
        MediaStore.MediaColumns.RELATIVE_PATH + "=? AND " + MediaStore.MediaColumns.DISPLAY_NAME + "=?",
        new String[] { rel, name }, null)) {
      if (c != null && c.moveToFirst()) return android.content.ContentUris.withAppendedId(coll, c.getLong(0));
    }
    return null;
  }

  private static void delete(ContentResolver cr, Uri coll, String rel, String name) {
    Uri u = find(cr, coll, rel, name);
    if (u != null) cr.delete(u, null, null);
  }
}
