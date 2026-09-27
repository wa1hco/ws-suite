/*
 * inhibit-test-gui — Windows GUI KEY-agent stand-in (docs/TX_INHIBIT.md s3)
 *
 * Installed as bin/inhibit-test-gui.exe next to wsjtx.exe.
 * Console sibling: inhibit-test (tools/inhibit-test/main.cpp) on all platforms.
 *
 * Same protocol and hang policy as inhibit-test:
 *   Hold sender: KEY assert -> hold (ttl_ms = hold_timeout_ms) + keepalives
 *                ~200 ms; EOT -> stop keepalives, then ttl_ms=0.
 *   KEYing monitor: break-in hang = 1.5 x word gap (10.5 x dit);
 *                   continuous KEY (long mark / non-break-in / SSB) hang = 0.
 *
 * KEY = grave/backtick ` (VK_OEM_3) or mouse on big button. Not Space.
 * Keys only while this window has focus (Windows delivers keys to focus).
 * Esc / Force RELEASE: end hold immediately (skip hang).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  IDC_HOST = 1001,
  IDC_PORT,
  IDC_STATION,
  IDC_BAND,
  IDC_TTL,
  IDC_KEEPALIVE,
  IDC_FIXED_HANG,
  IDC_HOLD_BTN,
  IDC_RELEASE_BTN,
  IDC_STATUS,
  IDC_LAST_PKT,
  IDC_COUNTERS,
  IDC_HINT,
  IDT_KEEPALIVE = 1,
  IDT_UI = 2,
  IDT_HANG = 3
};

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 22372
#define DEFAULT_TTL_MS 600
#define DEFAULT_KA_MS 200
#define TTL_MIN 100
#define TTL_MAX 30000

/* Hang = 1.5 x word gap = 10.5 x dit (docs/TX_INHIBIT.md s3.4); WPM 10..40 */
#define HANG_WORD_GAP_MULT 1.5
#define WORD_GAP_DITS 7
#define HANG_MIN_MS 315
#define HANG_MAX_MS 1260
#define CONTINUOUS_MARK_MS 500
#define MAX_INTRA_TX_GAP_DITS 5.0

enum KeyClass {
  KEY_CLASS_UNKNOWN = 0,
  KEY_CLASS_BREAK_IN,
  KEY_CLASS_CONTINUOUS
};

static HWND g_hwnd;
static HWND g_hold_btn;
static HWND g_status;
static HWND g_last_pkt;
static HWND g_counters;
static SOCKET g_sock = INVALID_SOCKET;
static int g_seq;
static int g_holds, g_releases, g_keepalives, g_errors;
static int g_udp_active;   /* Hold sender: hold/keepalives active */
static int g_key_down;     /* KEY level assert */
static int g_in_hang;      /* KEY open, hang before release hold */
static ULONGLONG g_key_down_at;
static ULONGLONG g_hang_until;
static int g_last_hang_ms;
static int g_last_mark_ms;

/* KEYing monitor (mirrors tools/inhibit-test KeyingMonitor) */
static int g_key_class;
static double g_dit_ms;
static int g_mark_open;
static ULONGLONG g_mark_start_ms;
static ULONGLONG g_gap_start_ms;

static void set_text(HWND h, const char *s) {
  if (h) SetWindowTextA(h, s ? s : "");
}

static void get_text(HWND parent, int id, char *buf, int n) {
  GetDlgItemTextA(parent, id, buf, n);
  buf[n - 1] = 0;
}

static int get_int_field(HWND parent, int id, int defv) {
  char b[64];
  get_text(parent, id, b, sizeof b);
  if (!b[0]) return defv;
  return atoi(b);
}

static int encode_datagram(char *out, int outn,
                           const char *station, const char *band,
                           int seq, int ttl_ms) {
  return snprintf(out, outn,
                  "{\"tx_inhibit\":1,\"ttl_ms\":%d,\"station\":\"%s\","
                  "\"band\":\"%s\",\"seq\":%d}",
                  ttl_ms, station, band, seq);
}

static void sanitize_token(char *s) {
  char *r = s, *w = s;
  for (; *r; ++r) {
    if (*r == '"' || *r == '\\' || (unsigned char)*r < 0x20) continue;
    *w++ = *r;
  }
  *w = 0;
}

static const char *class_name(int kc) {
  switch (kc) {
  case KEY_CLASS_BREAK_IN: return "break-in CW";
  case KEY_CLASS_CONTINUOUS: return "continuous (non-break-in/SSB)";
  default: return "unknown";
  }
}

static void keying_reset(void) {
  g_key_class = KEY_CLASS_UNKNOWN;
  g_dit_ms = 0.0;
  g_mark_open = 0;
  g_mark_start_ms = 0;
  g_gap_start_ms = 0;
}

static void note_dit_sample(double sample_ms) {
  if (sample_ms <= 0.0)
    return;
  if (g_dit_ms <= 0.0)
    g_dit_ms = sample_ms;
  else
    g_dit_ms = 0.35 * sample_ms + 0.65 * g_dit_ms;
}

static void note_mark(int mark_ms) {
  if (mark_ms <= 0)
    return;
  if (mark_ms >= CONTINUOUS_MARK_MS && g_key_class != KEY_CLASS_BREAK_IN) {
    g_key_class = KEY_CLASS_CONTINUOUS;
    return;
  }
  if (g_dit_ms <= 0.0) {
    if (mark_ms < CONTINUOUS_MARK_MS)
      note_dit_sample((double)mark_ms);
    return;
  }
  if ((double)mark_ms < 2.0 * g_dit_ms)
    note_dit_sample((double)mark_ms);
}

static void on_gap_closed(int gap_ms) {
  double max_gap;
  if (gap_ms <= 0)
    return;
  max_gap = (g_dit_ms > 0.0)
    ? MAX_INTRA_TX_GAP_DITS * g_dit_ms
    : (double)CONTINUOUS_MARK_MS;
  if ((double)gap_ms <= max_gap)
    g_key_class = KEY_CLASS_BREAK_IN;
}

static int hang_ms_break_in(void) {
  double hang;
  int h;
  if (g_dit_ms <= 0.0)
    return HANG_MIN_MS;
  hang = HANG_WORD_GAP_MULT * (double)WORD_GAP_DITS * g_dit_ms;
  h = (int)(hang + 0.5);
  if (h < HANG_MIN_MS) h = HANG_MIN_MS;
  if (h > HANG_MAX_MS) h = HANG_MAX_MS;
  return h;
}

static void keying_on_assert(ULONGLONG now) {
  if (g_gap_start_ms > 0) {
    int gap = (int)(now - g_gap_start_ms);
    if (gap < 0) gap = 0;
    on_gap_closed(gap);
    g_gap_start_ms = 0;
  }
  g_mark_open = 1;
  g_mark_start_ms = now;
}

/* Returns hang_ms after KEY open (0 = release hold now). */
static int keying_on_open(ULONGLONG now, int *out_mark_ms) {
  int mark_ms = 0;
  if (g_mark_open && g_mark_start_ms > 0) {
    mark_ms = (int)(now - g_mark_start_ms);
    if (mark_ms < 0) mark_ms = 0;
    note_mark(mark_ms);
  }
  g_mark_open = 0;
  g_mark_start_ms = 0;
  g_gap_start_ms = now;
  if (out_mark_ms)
    *out_mark_ms = mark_ms;

  if (g_key_class == KEY_CLASS_CONTINUOUS
      || (g_key_class == KEY_CLASS_UNKNOWN && mark_ms >= CONTINUOUS_MARK_MS)) {
    g_key_class = KEY_CLASS_CONTINUOUS;
    return 0;
  }
  if (g_key_class == KEY_CLASS_BREAK_IN || g_dit_ms > 0.0) {
    g_key_class = KEY_CLASS_BREAK_IN;
    return hang_ms_break_in();
  }
  if (mark_ms > 0 && mark_ms < CONTINUOUS_MARK_MS) {
    note_dit_sample((double)mark_ms);
    return hang_ms_break_in();
  }
  return 0;
}

static int send_datagram(HWND hwnd, int ttl_ms_arg, int is_keepalive,
                         const char *release_reason) {
  char host[128], station[64], band[32], portstr[32];
  char payload[512];
  char status[400];
  struct sockaddr_in addr;
  int port, ttl_ms, n, sent;

  get_text(hwnd, IDC_HOST, host, sizeof host);
  get_text(hwnd, IDC_STATION, station, sizeof station);
  get_text(hwnd, IDC_BAND, band, sizeof band);
  get_text(hwnd, IDC_PORT, portstr, sizeof portstr);

  if (!host[0]) strcpy(host, DEFAULT_HOST);
  if (!station[0]) strcpy(station, "TEST-KEY");
  if (!band[0]) strcpy(band, "144");
  sanitize_token(station);
  sanitize_token(band);
  port = portstr[0] ? atoi(portstr) : DEFAULT_PORT;
  if (port < 1 || port > 65535) {
    set_text(g_status, "ERROR: bad port");
    g_errors++;
    return 0;
  }

  if (ttl_ms_arg == 0) {
    ttl_ms = 0;
  } else {
    ttl_ms = get_int_field(hwnd, IDC_TTL, DEFAULT_TTL_MS);
    if (ttl_ms < TTL_MIN || ttl_ms > TTL_MAX) {
      set_text(g_status, "ERROR: hold_timeout_ms must be 100..30000");
      g_errors++;
      return 0;
    }
  }

  /* Race rule: never send keepalive after END_HOLD cleared g_udp_active */
  if (is_keepalive && !g_udp_active)
    return 0;

  g_seq++;
  n = encode_datagram(payload, sizeof payload, station, band, g_seq, ttl_ms);
  if (n <= 0 || n >= (int)sizeof payload) {
    set_text(g_status, "ERROR: encode failed");
    g_errors++;
    return 0;
  }

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short)port);
  addr.sin_addr.s_addr = inet_addr(host);
  if (addr.sin_addr.s_addr == INADDR_NONE) {
    struct hostent *he = gethostbyname(host);
    /* sockaddr_in holds IPv4 only; refuse non-AF_INET (e.g. IPv6) so we
     * never memcpy past sin_addr (was using he->h_length unchecked). */
    if (!he || he->h_addrtype != AF_INET
        || he->h_length != (int)sizeof(addr.sin_addr)
        || !he->h_addr_list || !he->h_addr_list[0]) {
      set_text(g_status, "ERROR: bad host");
      g_errors++;
      return 0;
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
  }

  sent = sendto(g_sock, payload, n, 0, (struct sockaddr *)&addr, sizeof addr);
  if (sent != n) {
    snprintf(status, sizeof status, "ERROR: sendto failed (%d)", WSAGetLastError());
    set_text(g_status, status);
    g_errors++;
    return 0;
  }

  set_text(g_last_pkt, payload);

  if (ttl_ms == 0) {
    g_releases++;
    if (release_reason && release_reason[0])
      snprintf(status, sizeof status,
               "RELEASE ttl_ms=0 -> %s:%d  (holds=%d ka=%d rel=%d)  (%s)",
               host, port, g_holds, g_keepalives, g_releases, release_reason);
    else
      snprintf(status, sizeof status,
               "RELEASE ttl_ms=0 -> %s:%d  (holds=%d ka=%d rel=%d)",
               host, port, g_holds, g_keepalives, g_releases);
  } else if (is_keepalive) {
    g_keepalives++;
    if (g_in_hang) {
      int hang_left = (int)(g_hang_until - GetTickCount64());
      if (hang_left < 0) hang_left = 0;
      snprintf(status, sizeof status,
               "KEEPALIVE (hang) -> %s:%d  hang left %d ms (hang was %d)",
               host, port, hang_left, g_last_hang_ms);
    } else {
      snprintf(status, sizeof status,
               "KEEPALIVE -> %s:%d  seq=%d  (holds=%d ka=%d rel=%d)",
               host, port, g_seq, g_holds, g_keepalives, g_releases);
    }
  } else {
    g_holds++;
    snprintf(status, sizeof status,
             "HOLD ttl_ms=%d -> %s:%d  (holds=%d ka=%d rel=%d)",
             ttl_ms, host, port, g_holds, g_keepalives, g_releases);
  }
  set_text(g_status, status);
  return 1;
}

static void update_counters(void) {
  char b[480];
  ULONGLONG now = GetTickCount64();
  const char *st;
  int hang_left = 0;

  if (g_key_down)
    st = "KEY-DOWN";
  else if (g_in_hang)
    st = "HANG";
  else if (g_udp_active)
    st = "UDP-ACTIVE";
  else
    st = "idle";

  if (g_in_hang && g_hang_until > now)
    hang_left = (int)(g_hang_until - now);

  if (g_in_hang) {
    snprintf(b, sizeof b,
             "state=%s  hang left=%d ms (applied %d)  class=%s  dit~%.0f ms  "
             "seq=%d  holds=%d ka=%d rel=%d err=%d",
             st, hang_left, g_last_hang_ms, class_name(g_key_class), g_dit_ms,
             g_seq, g_holds, g_keepalives, g_releases, g_errors);
  } else if (g_key_down) {
    int mark = (int)(now - g_key_down_at);
    if (mark < 0) mark = 0;
    snprintf(b, sizeof b,
             "state=%s  mark=%d ms  class=%s  seq=%d  "
             "holds=%d ka=%d rel=%d err=%d",
             st, mark, class_name(g_key_class), g_seq,
             g_holds, g_keepalives, g_releases, g_errors);
  } else {
    snprintf(b, sizeof b,
             "state=%s  last mark=%d ms  class=%s  dit~%.0f ms  seq=%d  "
             "holds=%d ka=%d rel=%d err=%d",
             st, g_last_mark_ms, class_name(g_key_class), g_dit_ms, g_seq,
             g_holds, g_keepalives, g_releases, g_errors);
  }
  set_text(g_counters, b);
}

static void set_key_ui(void) {
  if (g_key_down) {
    set_text(g_hold_btn, "KEY DOWN - release ` or mouse for hang / release hold");
  } else if (g_in_hang) {
    set_text(g_hold_btn, "HANG - keepalives until hang ends, then release hold");
  } else {
    set_text(g_hold_btn, "Hold ` (grave) or mouse = KEY (KEYing hang on release)");
  }
}

static void stop_timer(HWND hwnd, UINT id) {
  KillTimer(hwnd, id);
}

static void start_keepalive_timer(HWND hwnd) {
  int ka = get_int_field(hwnd, IDC_KEEPALIVE, DEFAULT_KA_MS);
  if (ka < 50) ka = 50;
  if (ka > 2000) ka = 2000;
  stop_timer(hwnd, IDT_KEEPALIVE);
  SetTimer(hwnd, IDT_KEEPALIVE, (UINT)ka, NULL);
}

/* Optional fixed hang override; empty field = KEYing monitor. */
static int fixed_hang_override(HWND hwnd, int *out_set) {
  char b[64];
  get_text(hwnd, IDC_FIXED_HANG, b, sizeof b);
  if (!b[0]) {
    if (out_set) *out_set = 0;
    return 0;
  }
  if (out_set) *out_set = 1;
  return atoi(b);
}

/* KEY assert: Hold sender + KEYing monitor */
static void key_down(HWND hwnd) {
  ULONGLONG now;
  if (g_key_down)
    return;
  stop_timer(hwnd, IDT_HANG);
  g_in_hang = 0;
  g_key_down = 1;
  now = GetTickCount64();
  g_key_down_at = now;
  keying_on_assert(now);

  if (!g_udp_active) {
    if (!send_datagram(hwnd, 1, 0, NULL)) {
      g_key_down = 0;
      return;
    }
    g_udp_active = 1;
    start_keepalive_timer(hwnd);
  } else {
    /* KEY re-assert during hang: keep hold, refresh packet */
    send_datagram(hwnd, 1, 0, NULL);
  }
  set_key_ui();
  update_counters();
}

/* END_HOLD: stop keepalives first, then ttl_ms=0 (one status line). */
static void finish_release(HWND hwnd, const char *reason) {
  stop_timer(hwnd, IDT_KEEPALIVE);
  stop_timer(hwnd, IDT_HANG);
  g_udp_active = 0;
  g_in_hang = 0;
  g_key_down = 0;
  send_datagram(hwnd, 0, 0, reason);
  keying_reset();
  set_key_ui();
  update_counters();
}

/* KEY open: hang (or hang 0), then release hold */
static void key_up(HWND hwnd) {
  int hang, mark_ms = 0, fixed_set = 0, fixed_hang;
  ULONGLONG now;
  char msg[200];

  if (!g_key_down)
    return;
  g_key_down = 0;
  now = GetTickCount64();
  hang = keying_on_open(now, &mark_ms);
  g_last_mark_ms = mark_ms;

  fixed_hang = fixed_hang_override(hwnd, &fixed_set);
  if (fixed_set) {
    hang = fixed_hang;
    if (hang < 0) hang = 0;
  }
  g_last_hang_ms = hang;

  if (!g_udp_active) {
    set_key_ui();
    update_counters();
    return;
  }

  if (hang <= 0) {
    snprintf(msg, sizeof msg, "KEY OPEN mark=%d ms hang=0 (%s)",
             mark_ms, class_name(g_key_class));
    set_text(g_status, msg);
    finish_release(hwnd, class_name(g_key_class));
    return;
  }

  g_in_hang = 1;
  g_hang_until = now + (ULONGLONG)hang;
  start_keepalive_timer(hwnd);
  stop_timer(hwnd, IDT_HANG);
  SetTimer(hwnd, IDT_HANG, (UINT)hang, NULL);
  if (g_dit_ms > 0.0)
    snprintf(msg, sizeof msg,
             "KEY OPEN mark=%d ms hang=%d ms class=%s dit~%.0f ms ~%.0f WPM",
             mark_ms, hang, class_name(g_key_class), g_dit_ms,
             1200.0 / g_dit_ms);
  else
    snprintf(msg, sizeof msg,
             "KEY OPEN mark=%d ms hang=%d ms class=%s",
             mark_ms, hang, class_name(g_key_class));
  set_text(g_status, msg);
  set_key_ui();
  update_counters();
}

/* Immediate clear - no hang (Escape / Force RELEASE). */
static void force_release(HWND hwnd) {
  g_key_down = 0;
  g_in_hang = 0;
  stop_timer(hwnd, IDT_HANG);
  if (g_udp_active)
    finish_release(hwnd, "force / Esc");
  else {
    keying_reset();
    set_text(g_status, "Force RELEASE (idle - nothing to clear)");
    set_key_ui();
    update_counters();
  }
}

static int is_edit_focus(HWND hwnd) {
  HWND f = GetFocus();
  int id;
  (void)hwnd;
  if (!f) return 0;
  id = GetDlgCtrlID(f);
  return id == IDC_HOST || id == IDC_PORT || id == IDC_STATION ||
         id == IDC_BAND || id == IDC_TTL || id == IDC_KEEPALIVE ||
         id == IDC_FIXED_HANG;
}

static HWND add_label(HWND p, int x, int y, int w, int h, const char *t) {
  return CreateWindowA("STATIC", t, WS_CHILD | WS_VISIBLE,
                       x, y, w, h, p, NULL, GetModuleHandle(NULL), NULL);
}

static HWND add_edit(HWND p, int id, int x, int y, int w, int h, const char *t) {
  return CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", t,
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                         x, y, w, h, p, (HMENU)(intptr_t)id,
                         GetModuleHandle(NULL), NULL);
}

static WNDPROC g_old_btn_proc;

static LRESULT CALLBACK HoldBtnProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  HWND parent = GetParent(hwnd);
  switch (msg) {
  case WM_LBUTTONDOWN:
    SetCapture(hwnd);
    key_down(parent);
    break;
  case WM_LBUTTONUP:
    if (GetCapture() == hwnd)
      ReleaseCapture();
    key_up(parent);
    break;
  case WM_CAPTURECHANGED:
    if ((HWND)lParam != hwnd && g_key_down)
      key_up(parent);
    break;
  }
  return CallWindowProcA(g_old_btn_proc, hwnd, msg, wParam, lParam);
}

static void layout(HWND hwnd) {
  int y = 12, x = 12, lw = 100, eh = 22, gap = 8;
  HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  HWND c;

  add_label(hwnd, x, y + 3, lw, eh, "Host");
  c = add_edit(hwnd, IDC_HOST, x + lw, y, 170, eh, DEFAULT_HOST);
  SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
  add_label(hwnd, x + 290, y + 3, 40, eh, "Port");
  c = add_edit(hwnd, IDC_PORT, x + 330, y, 70, eh, "22372");
  SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
  y += eh + gap;

  add_label(hwnd, x, y + 3, lw, eh, "Station");
  c = add_edit(hwnd, IDC_STATION, x + lw, y, 170, eh, "TEST-KEY");
  SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
  add_label(hwnd, x + 290, y + 3, 40, eh, "Band");
  c = add_edit(hwnd, IDC_BAND, x + 330, y, 70, eh, "144");
  SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
  y += eh + gap;

  add_label(hwnd, x, y + 3, lw, eh, "hold_timeout");
  c = add_edit(hwnd, IDC_TTL, x + lw, y, 70, eh, "600");
  SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
  add_label(hwnd, x + 185, y + 3, 50, eh, "ms");
  add_label(hwnd, x + 220, y + 3, 80, eh, "Keepalive ms");
  c = add_edit(hwnd, IDC_KEEPALIVE, x + 310, y, 70, eh, "200");
  SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
  y += eh + gap;

  add_label(hwnd, x, y + 3, lw, eh, "fixed hang ms");
  c = add_edit(hwnd, IDC_FIXED_HANG, x + lw, y, 70, eh, "");
  SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
  add_label(hwnd, x + 185, y + 3, 220, eh, "(empty = KEYing monitor)");
  y += eh + gap + 4;

  g_hold_btn = CreateWindowA(
      "BUTTON", "Hold ` (grave) or mouse = KEY (KEYing hang on release)",
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
      x, y, 480, 72, hwnd, (HMENU)(intptr_t)IDC_HOLD_BTN,
      GetModuleHandle(NULL), NULL);
  SendMessage(g_hold_btn, WM_SETFONT, (WPARAM)font, TRUE);
  g_old_btn_proc = (WNDPROC)SetWindowLongPtrA(
      g_hold_btn, GWLP_WNDPROC, (LONG_PTR)HoldBtnProc);
  y += 80;

  c = CreateWindowA("BUTTON", "Force RELEASE now (skip hang)",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                    x, y, 240, 28, hwnd, (HMENU)(intptr_t)IDC_RELEASE_BTN,
                    GetModuleHandle(NULL), NULL);
  SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);
  y += 36;

  add_label(hwnd, x, y, 480, eh, "Status");
  y += eh;
  g_status = CreateWindowA("STATIC",
                           "IDLE - press ` (grave) = KEY; release = hang then clear",
                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, 480, eh + 4, hwnd, (HMENU)(intptr_t)IDC_STATUS,
                           GetModuleHandle(NULL), NULL);
  SendMessage(g_status, WM_SETFONT, (WPARAM)font, TRUE);
  y += eh + gap;

  add_label(hwnd, x, y, 480, eh, "Last packet");
  y += eh;
  g_last_pkt = CreateWindowA("STATIC", "-",
                             WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                             x, y, 480, eh * 2, hwnd, (HMENU)(intptr_t)IDC_LAST_PKT,
                             GetModuleHandle(NULL), NULL);
  SendMessage(g_last_pkt, WM_SETFONT, (WPARAM)font, TRUE);
  y += eh * 2 + gap;

  g_counters = CreateWindowA("STATIC", "",
                             WS_CHILD | WS_VISIBLE | SS_LEFT,
                             x, y, 480, eh * 2, hwnd, (HMENU)(intptr_t)IDC_COUNTERS,
                             GetModuleHandle(NULL), NULL);
  SendMessage(g_counters, WM_SETFONT, (WPARAM)font, TRUE);
  y += eh * 2 + gap;

  c = CreateWindowA(
      "STATIC",
      "inhibit-test-gui (docs/TX_INHIBIT.md s3): Hold sender + KEYing monitor. "
      "KEY = grave ` (not Space) or mouse. Break-in: hang = 1.5x word gap "
      "(10.5x dit), clamp 315-1260 ms. Continuous mark >=500 ms: hang 0. "
      "Force RELEASE / Esc skips hang. Keys only when this window is focused. "
      "Same UDP as console inhibit-test.exe.",
      WS_CHILD | WS_VISIBLE | SS_LEFT,
      x, y, 480, eh * 5, hwnd, (HMENU)(intptr_t)IDC_HINT,
      GetModuleHandle(NULL), NULL);
  SendMessage(c, WM_SETFONT, (WPARAM)font, TRUE);

  update_counters();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
  case WM_CREATE:
    layout(hwnd);
    SetTimer(hwnd, IDT_UI, 50, NULL);
    return 0;

  case WM_COMMAND:
    if (LOWORD(wParam) == IDC_RELEASE_BTN && HIWORD(wParam) == BN_CLICKED) {
      force_release(hwnd);
      return 0;
    }
    break;

  case WM_TIMER:
    if (wParam == IDT_KEEPALIVE) {
      if (g_udp_active && (g_key_down || g_in_hang)) {
        send_datagram(hwnd, 1, 1, NULL);
        update_counters();
      } else {
        stop_timer(hwnd, IDT_KEEPALIVE);
      }
    } else if (wParam == IDT_HANG) {
      stop_timer(hwnd, IDT_HANG);
      if (g_in_hang && !g_key_down)
        finish_release(hwnd, "hang done / EOT");
    } else if (wParam == IDT_UI) {
      if (g_in_hang && !g_key_down && GetTickCount64() >= g_hang_until)
        finish_release(hwnd, "hang done / EOT");
      else
        update_counters();
    }
    return 0;

  case WM_DESTROY:
    if (g_udp_active)
      force_release(hwnd);
    stop_timer(hwnd, IDT_UI);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Only messages for this app's focused windows - not system-wide. */
static int filter_keys(MSG *m) {
  if (!g_hwnd) return 0;

  if (m->message == WM_KEYDOWN && m->wParam == VK_ESCAPE) {
    force_release(g_hwnd);
    return 1;
  }

  /* VK_OEM_3 = `~ on US keyboards (grave). Not Space. */
  if (m->message == WM_KEYDOWN && m->wParam == VK_OEM_3) {
    if (is_edit_focus(g_hwnd)) return 0;
    if (!(m->lParam & (1 << 30))) /* ignore autorepeat as re-assert */
      key_down(g_hwnd);
    return 1;
  }

  if (m->message == WM_KEYUP && m->wParam == VK_OEM_3) {
    if (is_edit_focus(g_hwnd) && !g_key_down) return 0;
    if (g_key_down)
      key_up(g_hwnd);
    return 1;
  }
  return 0;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
  WNDCLASSA wc;
  MSG msg;
  WSADATA wsa;
  (void)hp;
  (void)cmd;

  InitCommonControls();
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    MessageBoxA(NULL, "WSAStartup failed", "inhibit-test-gui", MB_ICONERROR);
    return 1;
  }
  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_sock == INVALID_SOCKET) {
    MessageBoxA(NULL, "UDP socket failed", "inhibit-test-gui", MB_ICONERROR);
    WSACleanup();
    return 1;
  }

  memset(&wc, 0, sizeof wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hi;
  wc.lpszClassName = "WsjtxInhibitTestGui";
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  if (!RegisterClassA(&wc)) {
    MessageBoxA(NULL, "RegisterClass failed", "inhibit-test-gui", MB_ICONERROR);
    return 1;
  }

  g_hwnd = CreateWindowExA(
      0, wc.lpszClassName,
      "inhibit-test-gui - KEY agent (grave ` KEY)",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 530, 560,
      NULL, NULL, hi, NULL);
  if (!g_hwnd) {
    MessageBoxA(NULL, "CreateWindow failed", "inhibit-test-gui", MB_ICONERROR);
    return 1;
  }

  ShowWindow(g_hwnd, show);
  UpdateWindow(g_hwnd);

  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    if (filter_keys(&msg))
      continue;
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }

  closesocket(g_sock);
  WSACleanup();
  return 0;
}
