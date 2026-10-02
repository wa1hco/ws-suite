#ifndef TX_INHIBIT_DROP_HPP__
#define TX_INHIBIT_DROP_HPP__

// Sole writer of the RTS or DTR pin.
//
//   pin = ptt_intent AND NOT inhibit
//
// ptt_intent is stored by the WSJT-X transceiver thread. inhibit is stored
// by the inhibit thread from message type 18. The inhibit thread samples
// both and writes the pin once per wake. rig_set_ptt() is not used.
//
// A drop that clears a pin that was high stores two monotonic times.
// t_rx_ns is taken when the type 18 datagram has been read. t_pin_ns is
// taken when the pin write returns. A sender on this machine that calls
// monotonic_ns() immediately before sendto() can subtract:
//   t_pin_ns - t_send  includes the thread wake
//   t_pin_ns - t_rx_ns is only the work after the socket read returns

#include <atomic>
#include <cstdint>
#include <mutex>

#include <QObject>
#include <QThread>
#include <QtGlobal>

#if defined(Q_OS_UNIX)
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
// winsock2.h must come before windows.h. Skip both when a Qt header
// already included windows.h, or this include warns under -Werror.
#  ifndef _INC_WINDOWS
#    ifndef _WINSOCK2API_
#      include <winsock2.h>
#    endif
#    include <windows.h>
#  endif
struct hamlib_port;
// Shared-port pin write on the handle Hamlib kept open. rig_set_ptt()
// stays bypassed. A separate PTT port is our own HANDLE.
extern "C" int ser_set_rts (struct hamlib_port * port, int state);
extern "C" int ser_set_dtr (struct hamlib_port * port, int state);
#endif

namespace TxInhibitDrop
{
  // Nanoseconds, comparable across processes on this machine.
  // Unix: CLOCK_MONOTONIC. Windows: QueryPerformanceCounter.
  inline qint64 monotonic_ns ()
  {
#if defined(Q_OS_UNIX)
    timespec ts {};
    if (clock_gettime (CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<qint64> (ts.tv_sec) * 1000000000LL
      + static_cast<qint64> (ts.tv_nsec);
#elif defined(Q_OS_WIN)
    static LONGLONG freq = 0;
    if (freq == 0)
      {
        LARGE_INTEGER f;
        if (!QueryPerformanceFrequency (&f) || f.QuadPart <= 0) return 0;
        freq = f.QuadPart;
      }
    LARGE_INTEGER c;
    if (!QueryPerformanceCounter (&c) || c.QuadPart < 0) return 0;
    unsigned long long const ticks = static_cast<unsigned long long> (c.QuadPart);
    unsigned long long const hz = static_cast<unsigned long long> (freq);
    unsigned long long const ns =
      (ticks / hz) * 1000000000ull + (ticks % hz) * 1000000000ull / hz;
    return static_cast<qint64> (ns);
#else
    return 0;
#endif
  }

  inline bool pin_level (bool intent, bool inhibit)
  {
    return intent && !inhibit;
  }

  inline std::atomic<bool> & ptt_intent ()
  {
    static std::atomic<bool> value {false};
    return value;
  }

  inline std::atomic<bool> & inhibit ()
  {
    static std::atomic<bool> value {false};
    return value;
  }

  inline std::atomic<std::uint64_t> & epoch ()
  {
    static std::atomic<std::uint64_t> value {0};
    return value;
  }

  inline std::atomic<int> & line_fd ()
  {
    static std::atomic<int> value {-1};
    return value;
  }

  // Windows: Hamlib's port object, which owns the COM handle. Unused on Unix.
  inline std::atomic<void *> & line_port ()
  {
    static std::atomic<void *> value {nullptr};
    return value;
  }

#if defined(Q_OS_WIN)
  // Selectors for ser_set_rts / ser_set_dtr. Values match Hamlib TIOCM_RTS/DTR.
  unsigned constexpr kPinRts = 0x004u;
  unsigned constexpr kPinDtr = 0x002u;
#endif

  inline std::atomic<unsigned> & line_bit ()
  {
    static std::atomic<unsigned> value {0};
    return value;
  }

  inline std::atomic<bool> & own_fd ()
  {
    static std::atomic<bool> value {false};
    return value;
  }

  inline std::atomic<QObject *> & worker ()
  {
    static std::atomic<QObject *> value {nullptr};
    return value;
  }

  // One-shot pair for the type 17 that reports this drop.
  struct PinStampPair
  {
    qint64 t_rx {0};
    qint64 t_pin {0};
  };

  inline PinStampPair & pin_stamps ()
  {
    static PinStampPair value;
    return value;
  }

  inline std::mutex & pin_stamp_mu ()
  {
    static std::mutex mu;
    return mu;
  }

  inline void publish_pin_stamps (qint64 t_rx, qint64 t_pin)
  {
    std::lock_guard<std::mutex> lock {pin_stamp_mu ()};
    pin_stamps ().t_rx = t_rx;
    pin_stamps ().t_pin = t_pin;
  }

  inline void take_pin_stamps (qint64 & t_rx, qint64 & t_pin)
  {
    std::lock_guard<std::mutex> lock {pin_stamp_mu ()};
    t_rx = pin_stamps ().t_rx;
    t_pin = pin_stamps ().t_pin;
    pin_stamps ().t_rx = 0;
    pin_stamps ().t_pin = 0;
    if (!(t_rx > 0 && t_pin > t_rx))
      {
        t_rx = 0;
        t_pin = 0;
      }
  }

  // True when a pin write was issued. The caller stamps the clock after it.
  inline bool write_pin (bool high)
  {
#if defined(Q_OS_UNIX)
    int const fd = line_fd ().load (std::memory_order_acquire);
    unsigned bit = line_bit ().load (std::memory_order_acquire);
    if (fd < 0 || bit == 0) return false;
#if defined(TIOCMBIS) && defined(TIOCMBIC)
    return ioctl (fd, high ? TIOCMBIS : TIOCMBIC, &bit) == 0;
#else
    unsigned lines = 0;
    if (ioctl (fd, TIOCMGET, &lines) != 0) return false;
    if (high) lines |= bit;
    else lines &= ~bit;
    return ioctl (fd, TIOCMSET, &lines) == 0;
#endif
#elif defined(Q_OS_WIN)
    void * token = line_port ().load (std::memory_order_acquire);
    unsigned const bit = line_bit ().load (std::memory_order_relaxed);
    bool const owned = own_fd ().load (std::memory_order_relaxed);
    if (!token || bit == 0) return false;
    if (owned)
      {
        // Separate PTT COM port. Hamlib closes that port after rig_open.
        auto handle = static_cast<HANDLE> (token);
        DWORD which = 0;
        if (bit == kPinRts) which = high ? SETRTS : CLRRTS;
        else if (bit == kPinDtr) which = high ? SETDTR : CLRDTR;
        if (which == 0) return false;
        return EscapeCommFunction (handle, which) != 0;
      }
    auto * port = static_cast<hamlib_port *> (token);
    int const rc = bit == kPinRts ? ser_set_rts (port, high ? 1 : 0)
                                  : bit == kPinDtr ? ser_set_dtr (port, high ? 1 : 0)
                                                   : -1;
    return rc == 0;
#else
    Q_UNUSED (high);
    return false;
#endif
  }

  // Inhibit thread only. Zero when no pin write ran.
  // The returned time is after the write returns.
  inline qint64 apply ()
  {
    bool const high = pin_level (ptt_intent ().load (std::memory_order_acquire),
                                 inhibit ().load (std::memory_order_acquire));
    if (!write_pin (high)) return 0;
    return monotonic_ns ();
  }

  inline void wake ()
  {
    QObject * w = worker ().load (std::memory_order_acquire);
    if (!w) return;
    QMetaObject::invokeMethod (w, "apply_pin", Qt::QueuedConnection);
  }

  inline void set_ptt_intent (bool on)
  {
    ptt_intent ().store (on, std::memory_order_release);
    wake ();
  }

  // Inhibit thread only. Bumps the epoch so a stale lease-expiry cannot
  // clear an inhibit that started after the expiry was queued.
  // t_rx_ns is monotonic_ns() at the type 18 socket read. Zero skips the
  // pin log. A stamp is stored only when this call drops a pin that was high.
  inline void set_inhibit_here (bool active, qint64 t_rx_ns = 0)
  {
    bool const was_high = pin_level (ptt_intent ().load (std::memory_order_acquire),
                                     inhibit ().load (std::memory_order_acquire));
    epoch ().fetch_add (1, std::memory_order_acq_rel);
    inhibit ().store (active, std::memory_order_release);
    qint64 const t_pin = apply ();
    bool const now_high = pin_level (ptt_intent ().load (std::memory_order_acquire),
                                     inhibit ().load (std::memory_order_acquire));
    if (t_rx_ns > 0 && t_pin > t_rx_ns && was_high && !now_high)
      {
        publish_pin_stamps (t_rx_ns, t_pin);
      }
  }

  inline void release_if_epoch (quint64 observed)
  {
    if (epoch ().load (std::memory_order_acquire) != observed) return;
    if (!inhibit ().load (std::memory_order_acquire)) return;
    inhibit ().store (false, std::memory_order_release);
    apply ();
  }

  inline void request_release ()
  {
    QObject * w = worker ().load (std::memory_order_acquire);
    if (!w) return;
    quint64 const observed = epoch ().load (std::memory_order_acquire);
    QMetaObject::invokeMethod (w, "release_if_epoch", Qt::QueuedConnection,
                               Q_ARG (quint64, observed));
  }

  inline void attach (QObject * w)
  {
    worker ().store (w, std::memory_order_release);
  }

  inline void publish (int fd, unsigned bit, bool owned)
  {
    line_bit ().store (bit, std::memory_order_release);
    own_fd ().store (owned, std::memory_order_release);
    line_port ().store (nullptr, std::memory_order_release);
    line_fd ().store (fd, std::memory_order_release);
    wake ();
  }

  // Shared COM port. Hamlib keeps the handle, so shutdown does not close it.
  inline void publish_port (void * port, unsigned bit)
  {
    line_bit ().store (bit, std::memory_order_relaxed);
    own_fd ().store (false, std::memory_order_relaxed);
    line_fd ().store (-1, std::memory_order_relaxed);
    line_port ().store (port, std::memory_order_release);
    wake ();
  }

  // Separate PTT COM port. Caller owns the HANDLE and shutdown closes it.
  inline void publish_handle (void * handle, unsigned bit)
  {
    line_bit ().store (bit, std::memory_order_relaxed);
    own_fd ().store (true, std::memory_order_relaxed);
    line_fd ().store (-1, std::memory_order_relaxed);
    line_port ().store (handle, std::memory_order_release);
    wake ();
  }

  // Inhibit thread, or the transceiver thread when the worker is already gone.
  inline void shutdown_here ()
  {
    ptt_intent ().store (false, std::memory_order_release);
    inhibit ().store (false, std::memory_order_release);
    write_pin (false);
    int const fd = line_fd ().exchange (-1, std::memory_order_acq_rel);
    void * token = line_port ().exchange (nullptr, std::memory_order_acq_rel);
    bool const owned = own_fd ().exchange (false, std::memory_order_acq_rel);
    line_bit ().store (0, std::memory_order_release);
#if defined(Q_OS_UNIX)
    Q_UNUSED (token);
    if (owned && fd >= 0) ::close (fd);
#elif defined(Q_OS_WIN)
    Q_UNUSED (fd);
    if (owned && token && token != INVALID_HANDLE_VALUE)
      CloseHandle (static_cast<HANDLE> (token));
#else
    Q_UNUSED (fd);
    Q_UNUSED (owned);
    Q_UNUSED (token);
#endif
  }

  inline void shutdown ()
  {
    QObject * w = worker ().load (std::memory_order_acquire);
    if (w && QThread::currentThread () != w->thread ())
      {
        QMetaObject::invokeMethod (w, "shutdown_pin", Qt::BlockingQueuedConnection);
        return;
      }
    shutdown_here ();
  }
}

#endif
