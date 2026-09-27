#ifndef UNIX_SIGNAL_HANDLER_HPP_
#define UNIX_SIGNAL_HANDLER_HPP_

/**
 * @brief Unix signal handler that bridges to Qt's event loop
 * 
 * This class handles Unix signals (SIGHUP, SIGTERM, SIGINT) and emits
 * Qt signals that can be connected to application shutdown slots.
 * 
 * Problem:
 * When WSJT-X receives a Unix signal (e.g., from `kill`, `systemctl stop`,
 * or terminal close), the process would terminate immediately without:
 * - Saving settings via closeEvent()
 * - Terminating the jt9 subprocess cleanly
 * - Detaching shared memory
 * - Cleaning up temp files
 * 
 * Solution:
 * This handler uses socket pairs to safely transfer from signal context
 * to Qt's event loop, allowing graceful shutdown.
 * 
 * Usage:
 * @code
 *   UnixSignalHandler unixSignalHandler;
 *   QObject::connect(&unixSignalHandler, &UnixSignalHandler::sigHUP, 
 *                    &mainWindow, &MainWindow::close);
 *   QObject::connect(&unixSignalHandler, &UnixSignalHandler::sigTERM,
 *                    &mainWindow, &MainWindow::close);
 *   QObject::connect(&unixSignalHandler, &UnixSignalHandler::sigINT,
 *                    &mainWindow, &MainWindow::close);
 * @endcode
 */

#include <QObject>

#if defined(Q_OS_UNIX) || defined(Q_OS_LINUX) || defined(Q_OS_MAC)

#include <QSocketNotifier>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

class UnixSignalHandler : public QObject
{
  Q_OBJECT

public:
  explicit UnixSignalHandler(QObject *parent = nullptr)
    : QObject(parent)
  {
    // Create socket pairs for each signal
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sighupFd_) != 0) {
      qWarning("UnixSignalHandler: Failed to create HUP socket pair");
    }
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigtermFd_) != 0) {
      qWarning("UnixSignalHandler: Failed to create TERM socket pair");
    }
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sigintFd_) != 0) {
      qWarning("UnixSignalHandler: Failed to create INT socket pair");
    }

    // Create socket notifiers
    snHup_ = new QSocketNotifier(sighupFd_[1], QSocketNotifier::Read, this);
    connect(snHup_, &QSocketNotifier::activated, this, &UnixSignalHandler::handleSigHup);
    
    snTerm_ = new QSocketNotifier(sigtermFd_[1], QSocketNotifier::Read, this);
    connect(snTerm_, &QSocketNotifier::activated, this, &UnixSignalHandler::handleSigTerm);
    
    snInt_ = new QSocketNotifier(sigintFd_[1], QSocketNotifier::Read, this);
    connect(snInt_, &QSocketNotifier::activated, this, &UnixSignalHandler::handleSigInt);

    // Install signal handlers
    struct sigaction sa;
    
    sa.sa_handler = UnixSignalHandler::hupSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGHUP, &sa, nullptr) != 0) {
      qWarning("UnixSignalHandler: Failed to install SIGHUP handler");
    }
    
    sa.sa_handler = UnixSignalHandler::termSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
      qWarning("UnixSignalHandler: Failed to install SIGTERM handler");
    }
    
    sa.sa_handler = UnixSignalHandler::intSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
      qWarning("UnixSignalHandler: Failed to install SIGINT handler");
    }
  }

  ~UnixSignalHandler()
  {
    // Close socket pairs
    ::close(sighupFd_[0]);
    ::close(sighupFd_[1]);
    ::close(sigtermFd_[0]);
    ::close(sigtermFd_[1]);
    ::close(sigintFd_[0]);
    ::close(sigintFd_[1]);
  }

  // Static signal handlers (called from signal context)
  // Note: In signal handlers, we intentionally ignore write() return values
  // as there's nothing we can safely do if the write fails in this context.
  static void hupSignalHandler(int)
  {
    char a = 1;
    ssize_t result __attribute__((unused)) = ::write(sighupFd_[0], &a, sizeof(a));
  }

  static void termSignalHandler(int)
  {
    char a = 1;
    ssize_t result __attribute__((unused)) = ::write(sigtermFd_[0], &a, sizeof(a));
  }

  static void intSignalHandler(int)
  {
    char a = 1;
    ssize_t result __attribute__((unused)) = ::write(sigintFd_[0], &a, sizeof(a));
  }

Q_SIGNALS:
  void sigHUP();   ///< Emitted when SIGHUP received (terminal hangup)
  void sigTERM();  ///< Emitted when SIGTERM received (termination request)
  void sigINT();   ///< Emitted when SIGINT received (Ctrl+C)

private Q_SLOTS:
  void handleSigHup()
  {
    snHup_->setEnabled(false);
    char tmp;
    ssize_t result __attribute__((unused)) = ::read(sighupFd_[1], &tmp, sizeof(tmp));
    
    emit sigHUP();
    
    snHup_->setEnabled(true);
  }

  void handleSigTerm()
  {
    snTerm_->setEnabled(false);
    char tmp;
    ssize_t result __attribute__((unused)) = ::read(sigtermFd_[1], &tmp, sizeof(tmp));
    
    emit sigTERM();
    
    snTerm_->setEnabled(true);
  }

  void handleSigInt()
  {
    snInt_->setEnabled(false);
    char tmp;
    ssize_t result __attribute__((unused)) = ::read(sigintFd_[1], &tmp, sizeof(tmp));
    
    emit sigINT();
    
    snInt_->setEnabled(true);
  }

private:
  static int sighupFd_[2];
  static int sigtermFd_[2];
  static int sigintFd_[2];

  QSocketNotifier *snHup_;
  QSocketNotifier *snTerm_;
  QSocketNotifier *snInt_;
};

// Note: Static member definitions are in UnixSignalHandler.cpp

#else // Windows - no-op implementation

class UnixSignalHandler : public QObject
{
  Q_OBJECT

public:
  explicit UnixSignalHandler(QObject *parent = nullptr)
    : QObject(parent)
  {
    // No-op on Windows
  }

Q_SIGNALS:
  void sigHUP();
  void sigTERM();
  void sigINT();
};

#endif // Q_OS_UNIX

#endif // UNIX_SIGNAL_HANDLER_HPP_
