#include "UdpDispatch.hpp"

#include <QNetworkInterface>
#include <QUdpSocket>
#include <QtEndian>

#include "NetworkMessage.hpp"
#include "TxInhibit/TxInhibitDrop.hpp"

UdpDispatchWorker::UdpDispatchWorker (QObject * parent)
  : QObject {parent}
{
}

void UdpDispatchWorker::init ()
{
  if (sock_) return;
  sock_ = new QUdpSocket {this};
  connect (sock_, &QUdpSocket::readyRead, this, &UdpDispatchWorker::read_pending);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  connect (sock_, &QUdpSocket::errorOccurred, this, &UdpDispatchWorker::on_error);
#else
  connect (sock_, static_cast<void (QUdpSocket::*) (QAbstractSocket::SocketError)> (&QUdpSocket::error),
           this, &UdpDispatchWorker::on_error);
#endif
}

void UdpDispatchWorker::shutdown ()
{
  if (!sock_) return;
  sock_->disconnect (this);
  sock_->close ();
  delete sock_;
  sock_ = nullptr;
}

void UdpDispatchWorker::do_set_id (QString id)
{
  id_ = id;
}

void UdpDispatchWorker::do_set_commands_enabled (bool enabled)
{
  commands_enabled_ = enabled;
}

void UdpDispatchWorker::do_bind (QString address)
{
  if (!sock_) return;
  QHostAddress const addr {address};
  if (sock_->state () != QAbstractSocket::UnconnectedState) sock_->close ();
  sock_->bind (addr);
  sock_->setSocketOption (QAbstractSocket::MulticastTtlOption, ttl_);
}

void UdpDispatchWorker::do_close ()
{
  if (sock_ && sock_->state () != QAbstractSocket::UnconnectedState) sock_->close ();
}

void UdpDispatchWorker::do_set_ttl (int ttl)
{
  ttl_ = ttl;
  if (sock_) sock_->setSocketOption (QAbstractSocket::MulticastTtlOption, ttl_);
}

void UdpDispatchWorker::do_write (QByteArray message, QString address, quint16 port, QString interface_name)
{
  if (!sock_) return;
  QHostAddress const dest {address};
  if (!interface_name.isEmpty ())
    {
      sock_->setMulticastInterface (QNetworkInterface::interfaceFromName (interface_name));
    }
  sock_->writeDatagram (message, dest, port);
}

QString UdpDispatchWorker::read_local_address () const
{
  if (!sock_ || sock_->state () == QAbstractSocket::UnconnectedState) return {};
  return sock_->localAddress ().toString ();
}

bool UdpDispatchWorker::is_unconnected () const
{
  return !sock_ || sock_->state () == QAbstractSocket::UnconnectedState;
}

bool UdpDispatchWorker::is_inhibit_hold (QByteArray const& msg) const
{
  if (msg.size () < 16) return false;
  auto const * bytes = reinterpret_cast<uchar const *> (msg.constData ());
  if (!commands_enabled_) return false;
  if (qFromBigEndian<quint32> (bytes) != NetworkMessage::Builder::magic) return false;
  if (qFromBigEndian<quint32> (bytes + 8) != NetworkMessage::TxInhibit) return false;
  quint32 const id_len = qFromBigEndian<quint32> (bytes + 12);
  if (id_len > 1024 || 16 + int (id_len) + 4 > msg.size ()) return false;
  if (QString::fromUtf8 (msg.constData () + 16, int (id_len)) != id_) return false;
  int off = 16 + int (id_len);
  quint32 const controller_len = qFromBigEndian<quint32> (bytes + off);
  off += 4 + int (controller_len);
  if (controller_len > 128 || off + 4 > msg.size ()) return false;
  quint32 const ttl = qFromBigEndian<quint32> (bytes + off);
  return ttl >= 100 && ttl <= 30000;
}

void UdpDispatchWorker::read_pending ()
{
  if (!sock_) return;
  while (sock_->hasPendingDatagrams ())
    {
      QByteArray data;
      data.resize (static_cast<int> (sock_->pendingDatagramSize ()));
      quint16 sender_port = 0;
      if (sock_->readDatagram (data.data (), data.size (), nullptr, &sender_port) < 0) continue;
      // Type 18 hold: clear RTS/DTR before this datagram is queued.
      if (is_inhibit_hold (data)) TxInhibitDrop::drop_direct ();
      Q_EMIT gui_datagram (data, sender_port);
    }
}

void UdpDispatchWorker::on_error ()
{
  if (!sock_) return;
#if defined (Q_OS_WIN)
  auto const e = sock_->error ();
  if (e == QAbstractSocket::NetworkError || e == QAbstractSocket::ConnectionRefusedError) return;
#endif
  Q_EMIT io_error (sock_->errorString ());
}

UdpDispatch::UdpDispatch ()
{
  worker_ = new UdpDispatchWorker;
  worker_->moveToThread (&thread_);
  connect (worker_, &UdpDispatchWorker::gui_datagram, this, &UdpDispatch::gui_datagram, Qt::QueuedConnection);
  connect (worker_, &UdpDispatchWorker::io_error, this, &UdpDispatch::io_error, Qt::QueuedConnection);
  thread_.setObjectName (QStringLiteral ("udp-dispatch"));
  thread_.start ();
  QMetaObject::invokeMethod (worker_, "init", Qt::BlockingQueuedConnection);
}

UdpDispatch::~UdpDispatch ()
{
  if (!worker_) return;
  QMetaObject::invokeMethod (worker_, "shutdown", Qt::BlockingQueuedConnection);
  thread_.quit ();
  thread_.wait ();
  delete worker_;
  worker_ = nullptr;
}

void UdpDispatch::set_id (QString const& id)
{
  QMetaObject::invokeMethod (worker_, "do_set_id", Qt::BlockingQueuedConnection, Q_ARG (QString, id));
}

void UdpDispatch::set_commands_enabled (bool enabled)
{
  QMetaObject::invokeMethod (worker_, "do_set_commands_enabled", Qt::BlockingQueuedConnection,
                             Q_ARG (bool, enabled));
}

void UdpDispatch::bind (QHostAddress const& address)
{
  QMetaObject::invokeMethod (worker_, "do_bind", Qt::BlockingQueuedConnection, Q_ARG (QString, address.toString ()));
}

void UdpDispatch::close_socket ()
{
  QMetaObject::invokeMethod (worker_, "do_close", Qt::BlockingQueuedConnection);
}

QString UdpDispatch::local_address () const
{
  QString addr;
  QMetaObject::invokeMethod (worker_, "read_local_address", Qt::BlockingQueuedConnection, Q_RETURN_ARG (QString, addr));
  return addr;
}

bool UdpDispatch::is_unconnected () const
{
  bool unconnected = true;
  QMetaObject::invokeMethod (worker_, "is_unconnected", Qt::BlockingQueuedConnection, Q_RETURN_ARG (bool, unconnected));
  return unconnected;
}

void UdpDispatch::set_ttl (int ttl)
{
  QMetaObject::invokeMethod (worker_, "do_set_ttl", Qt::BlockingQueuedConnection, Q_ARG (int, ttl));
}

void UdpDispatch::send_one (QByteArray const& message, QHostAddress const& address, quint16 port,
                            QString const& interface_name)
{
  QMetaObject::invokeMethod (worker_, "do_write", Qt::BlockingQueuedConnection,
                             Q_ARG (QByteArray, message),
                             Q_ARG (QString, address.toString ()),
                             Q_ARG (quint16, port),
                             Q_ARG (QString, interface_name));
}
