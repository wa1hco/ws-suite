#ifndef UDP_DISPATCH_HPP__
#define UDP_DISPATCH_HPP__

// Thread that owns the WSJT-X UDP socket (the heartbeat source).
//
// Inbound datagrams are read here, off the GUI thread. A type 18 hold
// clears RTS or DTR on this thread before the datagram is queued back
// for the existing parsers.

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

class QUdpSocket;

class UdpDispatchWorker final
  : public QObject
{
  Q_OBJECT

public:
  explicit UdpDispatchWorker (QObject * parent = nullptr);

public slots:
  void init ();
  void shutdown ();
  void do_set_id (QString id);
  void do_set_commands_enabled (bool enabled);
  void do_bind (QString address);
  void do_close ();
  void do_set_ttl (int ttl);
  void do_write (QByteArray message, QString address, quint16 port, QString interface_name);
  QString read_local_address () const;
  bool is_unconnected () const;

signals:
  void gui_datagram (QByteArray data, quint16 sender_port);
  void io_error (QString message);

private slots:
  void read_pending ();
  void on_error ();

private:
  bool is_inhibit_hold (QByteArray const& msg) const;

  QUdpSocket * sock_ {nullptr};
  QString id_;
  int ttl_ {1};
  bool commands_enabled_ {false};
};

class UdpDispatch final
  : public QObject
{
  Q_OBJECT

public:
  UdpDispatch ();
  ~UdpDispatch () override;

  void set_id (QString const& id);
  // Accept UDP requests. Type 18 is ignored, and the pin is not cleared,
  // while this is false. An in-flight hold is not cleared here.
  void set_commands_enabled (bool enabled);
  void bind (QHostAddress const& address);
  void close_socket ();
  QString local_address () const;
  bool is_unconnected () const;
  void set_ttl (int ttl);
  // Empty interface_name sends one unicast datagram. A name sends one
  // multicast datagram on that interface.
  void send_one (QByteArray const& message, QHostAddress const& address, quint16 port,
                 QString const& interface_name);

signals:
  void gui_datagram (QByteArray const& data, quint16 sender_port);
  void io_error (QString const& message);

private:
  QThread thread_;
  UdpDispatchWorker * worker_ {nullptr};
};

#endif
