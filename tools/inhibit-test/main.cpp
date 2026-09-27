// inhibit-test — KEY-agent stand-in for TX Inhibit (docs/TX_INHIBIT.md §3)
//
// Canonical name: inhibit-test (installed as bin/inhibit-test next to wsjtx).
// Formerly inhibit-spacebar. Implements two cooperating roles on one rare KEY:
//
//   KEY stand-in key: left quote / grave accent `  (not Space — typing
//   spaces must not false-trigger holds).
//
//   Hold sender   — hold immediately (ttl_ms = hold_timeout_ms), keepalives
//                   ~200 ms, release with ttl_ms=0 (cancel keepalives first).
//   KEYing monitor — classify break-in CW vs continuous KEY (non-break-in /
//                   SSB); measure dit; hang = 1.5× word gap (break-in only);
//                   EOT signals Hold sender to release.
//
// KEY level: press = assert, release = open (gap or EOT).
// Default input: only keys typed into *this* terminal (not other windows).
// Use --global-keys for system-wide KEY (true KEY-agent bench).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QHostAddress>
#include <QTimer>
#include <QUdpSocket>
#include <QByteArray>
#include <QString>
#include <QTextStream>
#include <QDateTime>
#include <QtGlobal>
#include <QSocketNotifier>

#include <algorithm>
#include <cmath>
#include <cstdio>

#if defined (Q_OS_WIN)
# include <windows.h>
# include <conio.h>
#elif defined (Q_OS_LINUX)
# include <fcntl.h>
# include <unistd.h>
# include <dirent.h>
# include <errno.h>
# include <linux/input.h>
# include <sys/ioctl.h>
# include <termios.h>
# include <cstring>
#elif defined (Q_OS_UNIX)
# include <unistd.h>
# include <termios.h>
# include <cstring>
#endif

namespace {

static int const kDefaultPort = 22372;
// hold_timeout_ms (wire ttl_ms) — safety on lost hold packets, not hang.
static int const kDefaultHoldTimeoutMs = 600;
static int const kKeepaliveMs = 200;

// Hang = 1.5 × word gap; word gap = 7 dits → hang = 10.5 × dit (Paris).
// WPM 10..40 ⇒ dit 120..30 ms ⇒ hang 1260..315 ms (docs/TX_INHIBIT.md §3.4).
static double const kHangWordGapMult = 1.5;
static int const kWordGapDits = 7;
static int const kHangMinMs = 315;   // ~40 WPM
static int const kHangMaxMs = 1260;  // ~10 WPM
// Continuous KEY (SSB / non-break-in): no element gaps → hang 0.
// Marks longer than this without short gaps are treated as continuous.
static int const kContinuousMarkMs = 500;
// Element gap upper bound while still "in character/word" (before hang EOT).
// Letter gap = 3 dits; use up to ~5 dits as "still break-in" open.
static double const kMaxIntraTxGapDits = 5.0;

QByteArray encode_hold (QString const& station, QString const& band,
                        qint64 seq, int ttl_ms)
{
  QByteArray body;
  body += "{\"tx_inhibit\":1,\"ttl_ms\":";
  body += QByteArray::number (ttl_ms);
  body += ",\"station\":\"";
  body += station.toUtf8 ();
  body += "\",\"band\":\"";
  body += band.toUtf8 ();
  body += "\",\"seq\":";
  body += QByteArray::number (seq);
  body += '}';
  return body;
}

// --- KEY level readers (grave/backtick ` — rare key, not Space) ------------
// Linux: KEY_GRAVE. Windows: VK_OEM_3 (US `~ key). Stdin: '`' (or '~').

enum class InputMode
{
  StdinTerminal,  // only keys typed into this terminal (has "focus")
  GlobalKeys      // system-wide keyboard (evdev / GetAsyncKeyState)
};

#if defined (Q_OS_LINUX)

static bool is_key_char (unsigned char c)
{
  return c == '`' || c == '~'; // grave / shifted grave on many layouts
}

#if defined (Q_OS_LINUX)
static int g_evdev_fd = -1;
#endif
static termios g_term_saved {};
static bool g_term_raw = false;
static bool g_stdin_space = false;
static bool g_stdin_quit = false;
// Set when the press was '~' (shifted grave) rather than '`'. The TTY byte
// distinguishes the two directly, which is more reliable than sampling the
// shift key at poll time.
static bool g_stdin_latch = false;

void restore_stdin_termios ()
{
  if (g_term_raw)
    {
      tcsetattr (STDIN_FILENO, TCSAFLUSH, &g_term_saved);
      g_term_raw = false;
    }
}

bool setup_stdin_raw ()
{
  if (!isatty (STDIN_FILENO))
    {
      return false;
    }
  if (tcgetattr (STDIN_FILENO, &g_term_saved) != 0)
    {
      return false;
    }
  termios raw = g_term_saved;
  // Input only: non-canonical, no echo, so `/q/Esc arrive as bytes when
  // this TTY has focus. Do NOT use cfmakeraw() — it clears c_oflag/OPOST so
  // '\n' is not mapped to CR+LF and QTextStream lines fail to wrap/return.
  raw.c_lflag &= static_cast<tcflag_t> (~(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG));
  raw.c_iflag &= static_cast<tcflag_t> (~(IXON | IXOFF | ICRNL));
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  // Leave c_oflag alone (keep OPOST / ONLCR) so stdout still wraps correctly.
  if (tcsetattr (STDIN_FILENO, TCSANOW, &raw) != 0)
    {
      return false;
    }
  g_term_raw = true;
  atexit (restore_stdin_termios);
  return true;
}

void poll_stdin_keys ()
{
  if (!g_term_raw)
    {
      return;
    }
  // Drain all pending bytes; TTY has no release event — only press bytes.
  unsigned char buf[64];
  for (;;)
    {
      ssize_t n = ::read (STDIN_FILENO, buf, sizeof (buf));
      if (n <= 0)
        {
          break;
        }
      for (ssize_t i = 0; i < n; ++i)
        {
          unsigned char c = buf[i];
          if (is_key_char (c))
            {
              // Grave press while this TTY has focus.
              g_stdin_space = true;
              if (c == '~')
                {
                  g_stdin_latch = true;
                }
            }
          else if (c == 'q' || c == 'Q' || c == 0x1b || c == 3 /* Ctrl-C */)
            {
              g_stdin_quit = true;
            }
        }
    }
}

// Degraded stdin KEY (only if /dev/input unavailable): TTY has no release.
// Synthesizes a short "down" after each press byte. Hold duration is NOT real
// KEY level: marks collapse to ~sticky length unless keyboard autorepeat keeps
// sending bytes. Prefer EVIOCGKEY (group 'input').
static int const kStdinStickyMs = 25;
static qint64 g_last_space_byte_ms = -1;

bool space_down_stdin (qint64 now_ms)
{
  poll_stdin_keys ();
  if (g_stdin_space)
    {
      g_stdin_space = false;
      g_last_space_byte_ms = now_ms;
      return true;
    }
  if (g_last_space_byte_ms >= 0 && (now_ms - g_last_space_byte_ms) < kStdinStickyMs)
    {
      return true;
    }
  g_last_space_byte_ms = -1;
  return false;
}

// Consume one TTY-focus KEY press edge (does not synthesize level).
// Used to *arm* EVIOCGKEY tracking only when this terminal has focus.
bool consume_stdin_key_press ()
{
  poll_stdin_keys ();
  if (!g_stdin_space)
    {
      return false;
    }
  g_stdin_space = false;
  return true;
}

// Returns (and clears) "the last grave press seen on this TTY was shifted".
bool take_stdin_latch_request ()
{
  bool const v = g_stdin_latch;
  g_stdin_latch = false;
  return v;
}

bool quit_requested_stdin ()
{
  poll_stdin_keys ();
  return g_stdin_quit;
}

#if defined (Q_OS_LINUX)
// Filled by open_evdev_keyboard() on failure (ASCII, for banner).
static QString g_evdev_fail_reason;

bool open_evdev_keyboard ()
{
  if (g_evdev_fd >= 0)
    {
      return true;
    }
  g_evdev_fail_reason.clear ();
  int n_open_fail_eacces = 0;
  int n_open_fail_other = 0;
  int n_no_grave = 0;
  int n_no_keys = 0;
  int n_tried = 0;
  char const * dirs[] = {"/dev/input/by-path", "/dev/input", nullptr};
  for (int d = 0; dirs[d]; ++d)
    {
      DIR * dir = opendir (dirs[d]);
      if (!dir)
        {
          continue;
        }
      while (dirent * ent = readdir (dir))
        {
          QString name = QString::fromLocal8Bit (ent->d_name);
          if (name == QLatin1String (".") || name == QLatin1String (".."))
            {
              continue;
            }
          if (d == 0 && !name.contains (QLatin1String ("event-kbd")))
            {
              continue;
            }
          if (d == 1 && !name.startsWith (QLatin1String ("event")))
            {
              continue;
            }
          QByteArray path = QByteArray (dirs[d]) + '/' + QByteArray (ent->d_name);
          ++n_tried;
          int fd = ::open (path.constData (), O_RDONLY | O_NONBLOCK);
          if (fd < 0)
            {
              if (errno == EACCES || errno == EPERM)
                {
                  ++n_open_fail_eacces;
                }
              else
                {
                  ++n_open_fail_other;
                }
              continue;
            }
          unsigned long evbits[(EV_MAX + 8 * sizeof (long) - 1) / (8 * sizeof (long))] = {};
          if (ioctl (fd, EVIOCGBIT (0, sizeof (evbits)), evbits) < 0)
            {
              ::close (fd);
              continue;
            }
          auto test_bit = [] (int bit, unsigned long const * arr) {
            return (arr[bit / (8 * sizeof (long))] >> (bit % (8 * sizeof (long)))) & 1UL;
          };
          if (!test_bit (EV_KEY, evbits))
            {
              ::close (fd);
              ++n_no_keys;
              continue;
            }
          unsigned long keybits[(KEY_MAX + 8 * sizeof (long) - 1) / (8 * sizeof (long))] = {};
          if (ioctl (fd, EVIOCGBIT (EV_KEY, sizeof (keybits)), keybits) == 0)
            {
              // Prefer keyboard that has grave/backtick (KEY_GRAVE).
              if (!test_bit (KEY_GRAVE, keybits))
                {
                  ::close (fd);
                  ++n_no_grave;
                  continue;
                }
            }
          g_evdev_fd = fd;
          closedir (dir);
          g_evdev_fail_reason.clear ();
          return true;
        }
      closedir (dir);
    }
  if (n_tried == 0)
    {
      g_evdev_fail_reason = QStringLiteral (
          "no /dev/input event-kbd devices found");
    }
  else if (n_open_fail_eacces > 0)
    {
      g_evdev_fail_reason = QStringLiteral (
          "permission denied on /dev/input (%1 devices; group 'input' required). "
          "Fix: sudo usermod -aG input $USER   then log out and back in "
          "(newgrp input is not enough for all sessions).")
          .arg (n_open_fail_eacces);
    }
  else if (n_no_grave > 0 && n_open_fail_other == 0)
    {
      g_evdev_fail_reason = QStringLiteral (
          "opened input devices but none advertise KEY_GRAVE");
    }
  else
    {
      g_evdev_fail_reason = QStringLiteral (
          "could not open a keyboard with KEY_GRAVE "
          "(tried=%1 eacces=%2 other_open=%3 no_key=%4 no_grave=%5)")
          .arg (n_tried).arg (n_open_fail_eacces).arg (n_open_fail_other)
          .arg (n_no_keys).arg (n_no_grave);
    }
  return false;
}

// True KEY *level* via kernel key state (no sticky). Call often.
// System-wide unless gated by the caller (focus-arm).
bool space_down_evdev ()
{
  if (g_evdev_fd < 0 && !open_evdev_keyboard ())
    {
      return false;
    }
  // Drain queue so EVIOCGKEY reflects current state
  input_event ev {};
  while (read (g_evdev_fd, &ev, sizeof (ev)) == static_cast<ssize_t> (sizeof (ev)))
    {
    }
  unsigned long keybits[(KEY_MAX + 8 * sizeof (long) - 1) / (8 * sizeof (long))] = {};
  if (ioctl (g_evdev_fd, EVIOCGKEY (sizeof (keybits)), keybits) < 0)
    {
      return false;
    }
  int const bit = KEY_GRAVE; // left quote / backtick `
  return (keybits[bit / (8 * sizeof (long))] >> (bit % (8 * sizeof (long)))) & 1UL;
}

// Shift held? Used only under --global-keys, where no TTY byte is available to
// tell '~' from '`'.
bool shift_down_evdev ()
{
  if (g_evdev_fd < 0)
    {
      return false;
    }
  unsigned long keybits[(KEY_MAX + 8 * sizeof (long) - 1) / (8 * sizeof (long))] = {};
  if (ioctl (g_evdev_fd, EVIOCGKEY (sizeof (keybits)), keybits) < 0)
    {
      return false;
    }
  auto down = [&] (int code) {
    return (keybits[code / (8 * sizeof (long))] >> (code % (8 * sizeof (long)))) & 1UL;
  };
  return down (KEY_LEFTSHIFT) || down (KEY_RIGHTSHIFT);
}

bool quit_requested_evdev ()
{
  if (g_evdev_fd < 0)
    {
      return false;
    }
  unsigned long keybits[(KEY_MAX + 8 * sizeof (long) - 1) / (8 * sizeof (long))] = {};
  if (ioctl (g_evdev_fd, EVIOCGKEY (sizeof (keybits)), keybits) < 0)
    {
      return false;
    }
  auto down = [&] (int code) {
    return (keybits[code / (8 * sizeof (long))] >> (code % (8 * sizeof (long)))) & 1UL;
  };
  return down (KEY_Q) || down (KEY_ESC);
}
#endif // Q_OS_LINUX

#elif defined (Q_OS_WIN)

// Windows KEY input
// ------------------
// Older builds used GetAsyncKeyState only when GetForegroundWindow() ==
// GetConsoleWindow(). That fails under Windows Terminal / ConPTY (and many VM
// consoles): the foreground HWND is the outer terminal frame, while
// GetConsoleWindow() is a different host window — so KEY and even q/Esc never
// fire. Linux uses TTY stdin to arm + true key level; mirror that here:
//
//   default: arm on ` / ~ / q / Esc read via _kbhit/_getch (this console only),
//            then track VK_OEM_3 with GetAsyncKeyState until release.
//   --global-keys: GetAsyncKeyState only (system-wide), no arm step.

static bool g_win_key_armed = false;
static bool g_win_stdin_quit = false;
static bool g_win_latch_req = false;

bool space_down_global ()
{
  // US/PC keyboard: VK_OEM_3 is the `~ key (left of 1). Rare in normal typing.
  return (GetAsyncKeyState (VK_OEM_3) & 0x8000) != 0;
}

// Shift held? Distinguishes '~' (latch) from '`' (momentary).
bool shift_down_global ()
{
  return (GetAsyncKeyState (VK_SHIFT) & 0x8000) != 0;
}

bool quit_requested_global ()
{
  return (GetAsyncKeyState ('Q') & 0x8000) != 0
    || (GetAsyncKeyState (VK_ESCAPE) & 0x8000) != 0;
}

// Drain console input buffer (only keys sent to *this* console session).
void poll_win_console_chars ()
{
  while (_kbhit ())
    {
      int c = _getch ();
      // Function/arrow keys produce a 0 or 0xE0 lead-in then a second code.
      if (c == 0 || c == 0xE0)
        {
          if (_kbhit ())
            {
              (void) _getch ();
            }
          continue;
        }
      if (c == '`' || c == '~')
        {
          g_win_key_armed = true;
          g_win_latch_req = (c == '~');
        }
      else if (c == 'q' || c == 'Q' || c == 27)
        {
          g_win_stdin_quit = true;
        }
    }
}

bool take_win_latch_request ()
{
  bool const v = g_win_latch_req;
  g_win_latch_req = false;
  return v;
}

// Default (this-console) KEY level: arm from console chars, then AsyncKeyState.
bool space_down_console ()
{
  poll_win_console_chars ();
  if (!g_win_key_armed)
    {
      return false;
    }
  if (space_down_global ())
    {
      return true;
    }
  // Key released — require another console ` / ~ to arm again.
  g_win_key_armed = false;
  return false;
}

bool quit_requested_console ()
{
  poll_win_console_chars ();
  if (g_win_stdin_quit)
    {
      return true;
    }
  // Also accept AsyncKeyState quit when console has focus (optional assist).
  HWND con = GetConsoleWindow ();
  HWND fg = GetForegroundWindow ();
  if (con && fg == con && quit_requested_global ())
    {
      return true;
    }
  return false;
}

#endif

// --- KEYing monitor (docs/TX_INHIBIT.md §3.2–3.5) ---------------------------

enum class KeyClass
{
  Unknown,      // not yet classified this transmission
  BreakInCw,    // saw short KEY opens (element/letter gaps)
  Continuous    // long mark, no short gaps → non-break-in CW / SSB
};

// Tracks KEY edges; decides hang (ms) when KEY opens (may be gap or EOT).
class KeyingMonitor
{
public:
  void reset_transmission ()
  {
    key_class_ = KeyClass::Unknown;
    dit_ms_ = 0.0;
    mark_open_ = false;
    mark_start_ms_ = -1;
    gap_start_ms_ = -1;
  }

  // KEY assert edge
  void on_key_assert (qint64 now_ms)
  {
    if (gap_start_ms_ >= 0)
      {
        int gap = static_cast<int> (now_ms - gap_start_ms_);
        on_gap_closed (gap);
        gap_start_ms_ = -1;
      }
    mark_open_ = true;
    mark_start_ms_ = now_ms;
  }

  // KEY open edge — returns hang_ms to wait before EOT (0 = release now).
  // If KEY asserts again before hang expires, call on_key_assert (not EOT).
  int on_key_open (qint64 now_ms)
  {
    int mark_ms = 0;
    if (mark_open_ && mark_start_ms_ >= 0)
      {
        mark_ms = static_cast<int> (now_ms - mark_start_ms_);
        note_mark (mark_ms);
      }
    mark_open_ = false;
    mark_start_ms_ = -1;
    gap_start_ms_ = now_ms;

    if (key_class_ == KeyClass::Continuous
        || (key_class_ == KeyClass::Unknown && mark_ms >= kContinuousMarkMs))
      {
        key_class_ = KeyClass::Continuous;
        return 0; // hang 0 — non-break-in / SSB
      }
    if (key_class_ == KeyClass::BreakInCw || dit_ms_ > 0.0)
      {
        key_class_ = KeyClass::BreakInCw;
        return hang_ms_break_in ();
      }
    // Unknown, short mark, no gaps yet: wait a short provisional hang based
    // on this mark as a dit candidate; if KEY returns, on_gap_closed fires.
    if (mark_ms > 0 && mark_ms < kContinuousMarkMs)
      {
        note_dit_sample (static_cast<double> (mark_ms));
        return hang_ms_break_in ();
      }
    return 0;
  }

  KeyClass key_class () const { return key_class_; }
  double dit_ms () const { return dit_ms_; }

  int hang_ms_break_in () const
  {
    if (dit_ms_ <= 0.0)
      {
        return kHangMinMs;
      }
    // hang = 1.5 × word_gap = 1.5 × 7 × dit = 10.5 × dit
    double hang = kHangWordGapMult * static_cast<double> (kWordGapDits) * dit_ms_;
    int h = static_cast<int> (std::lround (hang));
    return std::max (kHangMinMs, std::min (kHangMaxMs, h));
  }

  char const * class_name () const
  {
    switch (key_class_)
      {
      case KeyClass::BreakInCw: return "break-in CW";
      case KeyClass::Continuous: return "continuous (non-break-in/SSB)";
      default: return "unknown";
      }
  }

private:
  void note_mark (int mark_ms)
  {
    if (mark_ms <= 0)
      {
        return;
      }
    if (mark_ms >= kContinuousMarkMs && key_class_ != KeyClass::BreakInCw)
      {
        key_class_ = KeyClass::Continuous;
        return;
      }
    // Dit-like: shorter than ~2× current dit, or first short mark
    if (dit_ms_ <= 0.0)
      {
        if (mark_ms < kContinuousMarkMs)
          {
            note_dit_sample (static_cast<double> (mark_ms));
          }
        return;
      }
    if (mark_ms < 2.0 * dit_ms_)
      {
        note_dit_sample (static_cast<double> (mark_ms));
      }
    // else dah / long mark — speed from dits only
  }

  void note_dit_sample (double sample_ms)
  {
    if (sample_ms <= 0.0)
      {
        return;
      }
    if (dit_ms_ <= 0.0)
      {
        dit_ms_ = sample_ms;
      }
    else
      {
        dit_ms_ = 0.35 * sample_ms + 0.65 * dit_ms_;
      }
  }

  void on_gap_closed (int gap_ms)
  {
    if (gap_ms <= 0)
      {
        return;
      }
    // Short open between marks ⇒ break-in CW (element or letter gap).
    double max_gap = (dit_ms_ > 0.0)
      ? kMaxIntraTxGapDits * dit_ms_
      : static_cast<double> (kContinuousMarkMs);
    if (gap_ms <= max_gap)
      {
        key_class_ = KeyClass::BreakInCw;
      }
  }

  KeyClass key_class_ {KeyClass::Unknown};
  double dit_ms_ {0.0};
  bool mark_open_ {false};
  qint64 mark_start_ms_ {-1};
  qint64 gap_start_ms_ {-1};
};

} // namespace

int main (int argc, char * argv[])
{
  QCoreApplication app (argc, argv);
  QCoreApplication::setApplicationName (QStringLiteral ("inhibit-test"));
  QCoreApplication::setApplicationVersion (QStringLiteral ("2.1"));

  QCommandLineParser parser;
  parser.setApplicationDescription (
      QStringLiteral (
          "KEY-agent stand-in (docs/TX_INHIBIT.md s3): Hold sender + KEYing monitor.\n"
          "KEY key = left quote / grave ` (not Space - typing won't false-trigger).\n"
          "Break-in CW: hang = 1.5x word gap; continuous KEY: hang 0.\n"
          "Default: KEY only from *this terminal* (other windows ignored).\n"
          "Use --global-keys for system-wide KEY (true agent bench)."));
  parser.addHelpOption ();
  parser.addVersionOption ();
  QCommandLineOption hostOpt {QStringList () << "H" << "host",
                              QStringLiteral ("WSJT-X station host (default 127.0.0.1)"),
                              QStringLiteral ("host"),
                              QStringLiteral ("127.0.0.1")};
  QCommandLineOption portOpt {QStringList () << "p" << "port",
                              QStringLiteral ("Inhibit UDP port (default 22372)"),
                              QStringLiteral ("port"),
                              QString::number (kDefaultPort)};
  QCommandLineOption stationOpt {QStringList () << "s" << "station",
                                 QStringLiteral ("Badge station id"),
                                 QStringLiteral ("name"),
                                 QStringLiteral ("TEST-KEY")};
  QCommandLineOption bandOpt {QStringList () << "b" << "band",
                              QStringLiteral ("Band field (informational)"),
                              QStringLiteral ("band"),
                              QStringLiteral ("144")};
  QCommandLineOption ttlOpt {QStringList () << "t" << "ttl-ms",
                             QStringLiteral ("hold_timeout_ms on hold/keepalive packets (default 600)"),
                             QStringLiteral ("ms"),
                             QString::number (kDefaultHoldTimeoutMs)};
  QCommandLineOption fixedHangOpt {
    QStringList () << "fixed-hang-ms",
    QStringLiteral ("Override hang after KEY open (ms). Omit for KEYing-monitor hang."),
    QStringLiteral ("ms")};
  QCommandLineOption globalKeysOpt {
    QStringList () << "global-keys",
    QStringLiteral ("Read grave/` KEY system-wide (other windows can trigger). Not default.")};
  QCommandLineOption quietKaOpt {
    QStringList () << "verbose-keepalive",
    QStringLiteral ("Log every keepalive (default: HOLD, KEY events, RELEASE only).")};
  parser.addOption (hostOpt);
  parser.addOption (portOpt);
  parser.addOption (stationOpt);
  parser.addOption (bandOpt);
  parser.addOption (ttlOpt);
  parser.addOption (fixedHangOpt);
  parser.addOption (globalKeysOpt);
  parser.addOption (quietKaOpt);
  parser.process (app);

  QString const host = parser.value (hostOpt);
  quint16 const port = static_cast<quint16> (parser.value (portOpt).toUInt ());
  QString const station = parser.value (stationOpt);
  QString const band = parser.value (bandOpt);
  int const hold_timeout_ms = parser.value (ttlOpt).toInt ();
  bool const fixed_hang_set = parser.isSet (fixedHangOpt);
  int const fixed_hang_ms = fixed_hang_set ? parser.value (fixedHangOpt).toInt () : 0;
  bool const global_keys = parser.isSet (globalKeysOpt);
  bool const verbose_ka = parser.isSet (quietKaOpt);
  if (hold_timeout_ms < 100 || hold_timeout_ms > 30000)
    {
      QTextStream err (stderr);
      err << "ttl-ms (hold_timeout_ms) must be 100..30000\n";
      return 2;
    }

  // Some of these are only read inside platform-specific blocks below.
  [[maybe_unused]] InputMode input_mode = global_keys ? InputMode::GlobalKeys : InputMode::StdinTerminal;

  QString input_note;
  // Linux: require EVIOCGKEY (group 'input'). No stdin-sticky fallback - that
  // cannot measure KEY level and always reports ~40 WPM marks.
  [[maybe_unused]] bool use_evdev_level = false;
  [[maybe_unused]] bool focus_arm_evdev = false; // arm only after stdin grave when not --global-keys
#if defined (Q_OS_LINUX)
  if (!open_evdev_keyboard ())
    {
      QTextStream err (stderr);
      err << "inhibit-test: refusing to start (need true KEY level via /dev/input).\n"
          << "  " << (g_evdev_fail_reason.isEmpty ()
                      ? QStringLiteral ("could not open a keyboard device")
                      : g_evdev_fail_reason)
          << "\n"
          << "  Verify after re-login:  groups | grep -w input\n";
      return 1;
    }
  use_evdev_level = true;
  (void) setup_stdin_raw (); // quit always; KEY arm when not --global-keys
  if (input_mode == InputMode::GlobalKeys)
    {
      input_note = QStringLiteral (
          "KEY = grave/` via /dev/input EVIOCGKEY (true level, SYSTEM-WIDE). "
          "q/Esc also system-wide.");
    }
  else
    {
      // True level only after a grave press *into this terminal*, so other
      // windows cannot start a hold. Release tracked via EVIOCGKEY.
      focus_arm_evdev = true;
      input_note = QStringLiteral (
          "KEY = grave/` only when typed in *this terminal* "
          "(EVIOCGKEY level after TTY press; other windows ignored). "
          "q/Esc only from this terminal. --global-keys for system-wide KEY.");
    }
#elif defined (Q_OS_UNIX)
  QTextStream err_unix (stderr);
  err_unix << "inhibit-test: Linux /dev/input KEY level required; "
              "this non-Linux UNIX build is not supported.\n";
  return 1;
#elif defined (Q_OS_WIN)
  if (input_mode == InputMode::StdinTerminal)
    {
      input_note = QStringLiteral (
          "Windows: type ` / ~ into *this* console to KEY (then hold/release "
          "tracked via VK_OEM_3). q/Esc from this console. "
          "Works with Windows Terminal/ConPTY (not FG-window gated). "
          "--global-keys for system-wide KEY.");
    }
  else
    {
      input_note = QStringLiteral (
          "Windows: KEY = `~ (VK_OEM_3) SYSTEM-WIDE; q/Esc system-wide.");
    }
#else
  QTextStream err_plat (stderr);
  err_plat << "inhibit-test: unsupported platform for KEY level.\n";
  return 1;
#endif

  QUdpSocket sock;
  qint64 seq = 1;
  KeyingMonitor keying;   // KEYing monitor SM
  int holds_sent = 0;
  int keepalives_sent = 0;
  int releases_sent = 0;

  bool key_down = false;
  // '~' latch. The physical key is a momentary contact, so hold the latch here
  // and let everything downstream keep seeing a KEY *level*. A latched KEY reads
  // to the KEYing monitor as one long continuous mark, i.e. the non-break-in /
  // SSB class (hang 0) -- right by construction, since the operator ends the
  // transmission explicitly rather than by pausing. Break-in CW hang is still
  // exercised by the unshifted ` key, which stays momentary.
  bool latch_on = false;
  bool prev_key_raw = false;
  bool hold_active = false;   // Hold sender SM
  qint64 hang_until_ms = -1;  // EOT after KEY open (break-in hang)
  // Focus-arm: once a grave reaches this TTY, track EVIOCGKEY until KEY up.
  [[maybe_unused]] bool key_armed = false;
  [[maybe_unused]] qint64 key_arm_ms = -1;

  auto now_ms = [] () { return QDateTime::currentMSecsSinceEpoch (); };

  QTimer keepalive;
  keepalive.setInterval (kKeepaliveMs);

  // Hold sender: only path that emits UDP (race-safe vs keepalives).
  // release_reason: optional note on RELEASE lines only (one log line per packet).
  auto send_hold_packet = [&] (int ttl, bool is_keepalive,
                               char const * release_reason = nullptr) {
    if (is_keepalive && !hold_active)
      {
        return; // END_HOLD already cleared hold_active
      }
    QByteArray payload = encode_hold (station, band, seq++, ttl);
    qint64 n = sock.writeDatagram (payload, QHostAddress (host), port);
    QTextStream out (stdout);
    char const * tag = ttl == 0 ? "RELEASE" : (is_keepalive ? "KEEPALIVE" : "HOLD");
    if (ttl == 0)
      {
        ++releases_sent;
      }
    else if (is_keepalive)
      {
        ++keepalives_sent;
      }
    else
      {
        ++holds_sent;
      }
    if (verbose_ka || !is_keepalive || ttl == 0)
      {
        out << QDateTime::currentDateTime ().toString (QStringLiteral ("hh:mm:ss.zzz"))
            << "  " << tag
            << "  ttl_ms=" << ttl
            << "  -> " << host << ':' << port
            << "  (holds=" << holds_sent
            << " ka=" << keepalives_sent
            << " rel=" << releases_sent << ")";
        if (ttl == 0 && release_reason && release_reason[0])
          {
            out << "  (" << release_reason << ")";
          }
        out << '\n';
        out.flush ();
      }
    if (n < 0)
      {
        QTextStream err (stderr);
        err << "send failed: " << sock.errorString () << '\n';
      }
  };

  // END_HOLD: cancel keepalives first, then ttl_ms=0 (docs s3.6).
  auto end_hold = [&] (char const * reason) {
    hang_until_ms = -1;
    if (!hold_active)
      {
        return;
      }
    hold_active = false;
    keepalive.stop ();
    send_hold_packet (0, false, reason);
  };

  auto start_hold = [&] () {
    if (hold_active)
      {
        return;
      }
    hold_active = true;
    send_hold_packet (hold_timeout_ms, false);
    keepalive.start ();
  };

  // 5 ms poll when true key level is available (evdev / Windows).
  int const poll_ms =
#if defined (Q_OS_LINUX)
      use_evdev_level ? 5 : 10;
#elif defined (Q_OS_WIN)
      5;
#else
      10;
#endif

  // Banner uses ASCII only so logs stay readable on non-UTF-8 terminals.
  QTextStream out (stdout);
  out << "inhibit-test (KEY agent) -> " << host << ':' << port << '\n'
      << "  ` (grave) = KEY level: hold to assert, release to open  - not Space\n"
      << "  ~ (shift+grave) = LATCH on; press ` or ~ again to release\n"
      << "  q or Esc  = release hold and quit\n"
      << "  station=" << station << "  band=" << band << '\n'
      << "  hold_timeout_ms=" << hold_timeout_ms
      << "  keepalive every " << kKeepaliveMs << " ms while hold active\n"
      << "  key poll=" << poll_ms << " ms"
#if defined (Q_OS_LINUX)
      << (use_evdev_level ? " (EVIOCGKEY true level)" : " (stdin sticky fallback)")
#endif
      << '\n'
      << "  hang=" << (fixed_hang_set
                       ? QStringLiteral ("fixed override %1 ms").arg (std::max (0, fixed_hang_ms))
                       : QStringLiteral ("KEYing monitor: break-in 1.5x word gap; continuous hang=0"))
      << '\n'
      << "  " << input_note << '\n'
      << "  Latched (~) reads as one continuous mark: hang=0, released on the next press.\n"
      << "  Held (`) >=500 ms is also continuous; short taps use break-in hang.\n"
      << "  Or: --fixed-hang-ms 0 for immediate release on KEY open.\n"
      << "  See docs/TX_INHIBIT.md s3 (Hold sender + KEYing monitor).\n\n";
  out.flush ();

  QObject::connect (&keepalive, &QTimer::timeout, &app, [&] () {
      if (hold_active)
        {
          send_hold_packet (hold_timeout_ms, true);
        }
    });

  QTimer poll;
  poll.setInterval (poll_ms);
  QObject::connect (&poll, &QTimer::timeout, &app, [&] () {
      qint64 t = now_ms ();

      bool want_quit = false;
      bool raw_space = false;
      // True when this press was '~' (shifted) rather than '`'.
      bool latch_request = false;
#if defined (Q_OS_LINUX)
      // KEY (grave):
      //   --global-keys: EVIOCGKEY always (system-wide).
      //   default: arm only on grave typed into *this* TTY, then track
      //            EVIOCGKEY level until key up (true release timing, no
      //            false holds from other windows).
      // Quit: stdin only unless --global-keys.
      if (use_evdev_level)
        {
          if (input_mode == InputMode::GlobalKeys)
            {
              raw_space = space_down_evdev ();
              latch_request = raw_space && shift_down_evdev ();
              want_quit = quit_requested_evdev () || quit_requested_stdin ();
            }
          else if (focus_arm_evdev)
            {
              // Arm only when grave is typed into *this* TTY (other windows
              // never arm). Then track EVIOCGKEY for true release timing.
              if (consume_stdin_key_press ())
                {
                  key_armed = true;
                  key_arm_ms = t;
                  // The TTY byte distinguishes '~' from '`' directly.
                  latch_request = take_stdin_latch_request ();
                }
              if (key_armed)
                {
                  bool const ev = space_down_evdev ();
                  if (ev)
                    {
                      raw_space = true;
                    }
                  else if (key_arm_ms >= 0
                           && (t - key_arm_ms) < kStdinStickyMs)
                    {
                      // Just armed: keep down briefly if kernel lag / short tap.
                      raw_space = true;
                    }
                  else
                    {
                      // KEY open — disarm; next KEY needs TTY focus again.
                      raw_space = false;
                      key_armed = false;
                      key_arm_ms = -1;
                    }
                }
              else
                {
                  raw_space = false;
                }
              want_quit = quit_requested_stdin ();
            }
          else
            {
              raw_space = space_down_evdev ();
              latch_request = raw_space && shift_down_evdev ();
              want_quit = quit_requested_stdin ();
            }
        }
      else
        {
          raw_space = space_down_stdin (t);
          if (raw_space)
            {
              latch_request = take_stdin_latch_request ();
            }
          want_quit = quit_requested_stdin ();
        }
#elif defined (Q_OS_UNIX)
      // Non-Linux UNIX refuses to start (see the input-setup block
      // above), so there is nothing to poll; the TTY helpers are
      // compiled only on Linux.
      Q_UNUSED (t);
#elif defined (Q_OS_WIN)
      if (input_mode == InputMode::StdinTerminal)
        {
          // Console arm sets latch_request from '~' byte; level uses VK_OEM_3.
          raw_space = space_down_console ();
          if (raw_space)
            {
              latch_request = take_win_latch_request () || shift_down_global ();
            }
          want_quit = quit_requested_console ();
        }
      else
        {
          raw_space = space_down_global ();
          latch_request = raw_space && shift_down_global ();
          want_quit = quit_requested_global ();
        }
#else
      Q_UNUSED (t);
#endif

      // Two behaviours on one physical key, always both available:
      //   `  (unshifted) momentary - KEY follows the key, as a real KEY line
      //                  does; this is what break-in CW classification needs.
      //   ~  (shifted)   latches   - the hold stays asserted after the key is
      //                  released, freeing the keyboard to drive WSJT-X.
      // Either key, pressed again, clears the latch.
      bool const key_rising = raw_space && !prev_key_raw;
      if (key_rising)
        {
          if (latch_on)
            {
              latch_on = false;      // second press releases, ` or ~ alike
            }
          else if (latch_request)
            {
              latch_on = true;       // ~ latches on
            }
        }
      prev_key_raw = raw_space;
      bool space = latch_on || raw_space;

      if (want_quit)
        {
          latch_on = false;
          end_hold ("quit");
          out << "quit\n";
          out.flush ();
#if defined (Q_OS_LINUX)
          restore_stdin_termios ();
#endif
          QCoreApplication::quit ();
          return;
        }

      // --- KEY assert edge → Hold sender + KEYing monitor
      if (space && !key_down)
        {
          key_down = true;
          hang_until_ms = -1;
          keying.on_key_assert (t);
          start_hold ();
          out << QDateTime::currentDateTime ().toString (QStringLiteral ("hh:mm:ss.zzz"))
              << "  KEY ASSERT  class=" << keying.class_name () << '\n';
          out.flush ();
        }

      // --- KEY open edge → hang (or EOT if hang=0)
      if (!space && key_down)
        {
          key_down = false;
          int hang_ms = keying.on_key_open (t);
          if (fixed_hang_set)
            {
              hang_ms = std::max (0, fixed_hang_ms);
            }
          QString const cls = QString::fromUtf8 (keying.class_name ());
          double const dit = keying.dit_ms ();
          if (hang_ms <= 0)
            {
              // Continuous KEY / hang 0: EOT immediately (may be before first keepalive)
              end_hold (keying.class_name ());
              keying.reset_transmission ();
              out << QDateTime::currentDateTime ().toString (QStringLiteral ("hh:mm:ss.zzz"))
                  << "  KEY OPEN   hang=0  (" << cls << ")\n";
              out.flush ();
            }
          else
            {
              hang_until_ms = t + hang_ms;
              out << QDateTime::currentDateTime ().toString (QStringLiteral ("hh:mm:ss.zzz"))
                  << "  KEY OPEN   hang=" << hang_ms << " ms"
                  << "  class=" << cls;
              if (dit > 0.0)
                {
                  out << "  dit~" << qRound (dit) << " ms"
                      << "  ~" << qRound (1200.0 / dit) << " WPM";
                }
              out << '\n';
              out.flush ();
            }
        }

      // --- hang complete → EOT → release hold
      if (!key_down && hang_until_ms >= 0 && t >= hang_until_ms)
        {
          end_hold ("hang done / EOT");
          keying.reset_transmission ();
        }
    });
  poll.start ();

  int rc = app.exec ();
#if defined (Q_OS_LINUX)
  restore_stdin_termios ();
#endif
  return rc;
}
