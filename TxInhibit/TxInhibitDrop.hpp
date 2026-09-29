#ifndef TX_INHIBIT_DROP_HPP__
#define TX_INHIBIT_DROP_HPP__

// Sole writer of the RTS or DTR pin.
//
//   pin = ptt_intent AND NOT inhibit
//
// ptt_intent is stored by the WSJT-X transceiver thread. inhibit is stored
// by the inhibit thread from message type 18. The inhibit thread samples
// both and writes the pin once per wake. rig_set_ptt() is not used.

#include <atomic>
#include <cstdint>

#include <QObject>
#include <QThread>
#include <QtGlobal>

#if defined(Q_OS_UNIX)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace TxInhibitDrop
{
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

  inline void write_pin (bool high)
  {
#if defined(Q_OS_UNIX)
    int const fd = line_fd ().load (std::memory_order_acquire);
    unsigned bit = line_bit ().load (std::memory_order_acquire);
    if (fd < 0 || bit == 0) return;
#if defined(TIOCMBIS) && defined(TIOCMBIC)
    ioctl (fd, high ? TIOCMBIS : TIOCMBIC, &bit);
#else
    unsigned lines = 0;
    if (ioctl (fd, TIOCMGET, &lines) != 0) return;
    if (high) lines |= bit;
    else lines &= ~bit;
    ioctl (fd, TIOCMSET, &lines);
#endif
#else
    Q_UNUSED (high);
#endif
  }

  // Inhibit thread only.
  inline void apply ()
  {
    bool const high = pin_level (ptt_intent ().load (std::memory_order_acquire),
                                 inhibit ().load (std::memory_order_acquire));
    write_pin (high);
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
  inline void set_inhibit_here (bool active)
  {
    epoch ().fetch_add (1, std::memory_order_acq_rel);
    inhibit ().store (active, std::memory_order_release);
    apply ();
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
    line_fd ().store (fd, std::memory_order_release);
    wake ();
  }

  // Inhibit thread, or the transceiver thread when the worker is already gone.
  inline void shutdown_here ()
  {
    ptt_intent ().store (false, std::memory_order_release);
    inhibit ().store (false, std::memory_order_release);
    write_pin (false);
    int const fd = line_fd ().exchange (-1, std::memory_order_acq_rel);
    bool const owned = own_fd ().exchange (false, std::memory_order_acq_rel);
    line_bit ().store (0, std::memory_order_release);
#if defined(Q_OS_UNIX)
    if (owned && fd >= 0) ::close (fd);
#else
    Q_UNUSED (fd);
    Q_UNUSED (owned);
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
