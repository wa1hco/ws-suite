#ifndef TX_INHIBIT_DROP_HPP__
#define TX_INHIBIT_DROP_HPP__

// Clear RTS or DTR without taking the Hamlib rig lock.
// Linux: TIOCMBIC on the PTT file descriptor copied at rig open.
// Windows: Hamlib ser_set_rts / ser_set_dtr (EscapeCommFunction).

#include <atomic>

#include <QtGlobal>

#if defined(Q_OS_UNIX)
#include <sys/ioctl.h>
#endif

namespace TxInhibitDrop
{
  typedef int (*LineDrop) (void * port);

  inline std::atomic<int> & line_fd ()
  {
    static std::atomic<int> value (-1);
    return value;
  }

  inline std::atomic<unsigned> & line_bit ()
  {
    static std::atomic<unsigned> value (0);
    return value;
  }

  inline std::atomic<LineDrop> & line_drop ()
  {
    static std::atomic<LineDrop> value (static_cast<LineDrop> (0));
    return value;
  }

  inline std::atomic<void *> & line_port ()
  {
    static std::atomic<void *> value (static_cast<void *> (0));
    return value;
  }

  inline void clear ()
  {
    line_port ().store (static_cast<void *> (0), std::memory_order_release);
    line_drop ().store (static_cast<LineDrop> (0), std::memory_order_release);
    line_fd ().store (-1, std::memory_order_release);
    line_bit ().store (0, std::memory_order_release);
  }

  // fd/bit are stored before the optional Hamlib callback.
  inline void publish (int fd, unsigned bit, LineDrop drop, void * port)
  {
    if (fd < 0 || bit == 0)
      {
        clear ();
        return;
      }
    line_bit ().store (bit, std::memory_order_release);
    line_fd ().store (fd, std::memory_order_release);
    line_drop ().store (drop, std::memory_order_release);
    line_port ().store (port, std::memory_order_release);
  }

  inline void drop_direct ()
  {
#if defined(Q_OS_UNIX)
    int const fd = line_fd ().load (std::memory_order_acquire);
    unsigned bit = line_bit ().load (std::memory_order_acquire);
    if (fd >= 0 && bit != 0)
      {
#if defined(TIOCMBIC)
        ioctl (fd, TIOCMBIC, &bit);
#else
        unsigned lines = 0;
        if (ioctl (fd, TIOCMGET, &lines) == 0)
          {
            lines &= ~bit;
            ioctl (fd, TIOCMSET, &lines);
          }
#endif
      }
#endif
    void * port = line_port ().load (std::memory_order_acquire);
    LineDrop drop = line_drop ().load (std::memory_order_acquire);
    if (port && drop) (void) drop (port);
  }
}

#endif
