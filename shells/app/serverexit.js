//
// WHAT A GAME SERVER'S GOING AWAY MEANS, judged by the process that went --
// not by a flag about whichever process the shell happens to hold now.
//
// The shell restarts its server for every play-test: it stops the old one,
// waits for its 'exit', starts the new one. The old one's 'close' -- which is
// what main.js listens to, because by then all of its output has arrived --
// comes AFTER 'exit', and so often after the restart had finished and cleared
// the shell's "restarting" flag. The old server's intended stop was then read
// as the new one failing: an error box quoting the old server's log, or the
// whole app quitting, on the first Play after a module or a map was changed.
//
//   'expected'        the shell stopped it, the app is quitting, or it is
//                     not the server the shell is running any more
//   'back-to-studio'  a studio play-test's server failed: show the studio
//   'fatal'           the game's own server failed: say so, and quit
//
export function exitVerdict ({ intended, current, quitting, playTest, studio }) {
  if (quitting || intended || !current) return 'expected'
  if (playTest && studio) return 'back-to-studio'
  return 'fatal'
}
