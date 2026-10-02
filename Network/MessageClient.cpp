#include "MessageClient.hpp"

#include <stdexcept>
#include <vector>
#include <algorithm>
#include <limits>

#include <QNetworkInterface>
#include <QHostInfo>
#include <QTimer>
#include <QQueue>
#include <QByteArray>
#include <QDataStream>
#include <QColor>
#include <QDebug>
#include <QtEndian>

#include "NetworkMessage.hpp"
#include "UdpDispatch.hpp"
#include "TxInhibit/TxInhibitDrop.hpp"
#include "qt_helpers.hpp"
#include "pimpl_impl.hpp"

#include "moc_MessageClient.cpp"

// some trace macros
#if WSJT_TRACE_UDP
#define TRACE_UDP(MSG) qDebug () << QString {"MessageClient::%1:"}.arg (__func__) << MSG
#else
#define TRACE_UDP(MSG)
#endif

namespace
{
  int constexpr inhibit_message_limit {4096};

  bool read_inhibit_text (QDataStream& in, QString& text, quint32 limit, bool identity)
  {
    quint32 length {0};
    in >> length;
    if (length == 0xffffffff && !identity) length = 0;
    if (in.status () != QDataStream::Ok || length > limit
        || length > quint64 (in.device ()->bytesAvailable ())) return false;
    QByteArray bytes (int (length), '\0');
    if (in.readRawData (bytes.data (), bytes.size ()) != bytes.size ()) return false;
    text = QString::fromUtf8 (bytes);
    if (text.toUtf8 () != bytes || (identity && text.isEmpty ())) return false;
    for (auto character : text.toUcs4 ())
      {
        if (!QChar::isPrint (character) || (identity && QChar::isSpace (character))) return false;
      }
    return true;
  }
}

class MessageClient::impl
  : public QObject
{
  Q_OBJECT;

public:
  impl (QString const& id, QString const& version, QString const& revision,
        port_type server_port, int TTL, MessageClient * self)
    : QObject {}
    , self_ {self}
    , enabled_ {false}
    , id_ {id}
    , version_ {version}
    , revision_ {revision}
    , dns_lookup_id_ {-1}
    , server_port_ {server_port}
    , TTL_ {TTL}
    , schema_ {2}  // use 2 prior to negotiation not 1 which is broken
    , heartbeat_timer_ {new QTimer {this}}
  {
    connect (heartbeat_timer_, &QTimer::timeout, this, &impl::heartbeat);
    dispatch_.set_id (id_);
    dispatch_.set_ttl (TTL_);
    connect (&dispatch_, &UdpDispatch::gui_datagram, this, &impl::pending_datagrams,
             Qt::QueuedConnection);
    connect (&dispatch_, &UdpDispatch::io_error, this, [this] (QString const& message) {
        Q_EMIT self_->error (message);
      });
    inhibit_invalid_timer_.setParent (this);
    inhibit_invalid_timer_.setSingleShot (true);
    connect (&inhibit_invalid_timer_, &QTimer::timeout, this, [this] {
        auto const count = inhibit_invalid_count_;
        inhibit_invalid_count_ = 0;
        Q_EMIT self_->tx_inhibit_invalid (count);
      });

    heartbeat_timer_->start (NetworkMessage::pulse * 1000);
  }

  ~impl ()
  {
    closedown ();
    if (dns_lookup_id_ != -1)
      {
        QHostInfo::abortHostLookup (dns_lookup_id_);
      }
  }

  enum StreamStatus {Fail, Short, OK};

  void set_server (QString const& server_name, QStringList const& network_interface_names);
  Q_SLOT void host_info_results (QHostInfo);
  void start ();
  void parse_message (QByteArray const&);
  void pending_datagrams (QByteArray const& datagram, quint16 sender_port);
  bool parse_tx_inhibit (QByteArray const&);
  void invalid_inhibit ();
  bool inhibit_available () const;
  void update_inhibit_status (bool supported, bool inhibited, QString const& source_station,
                              quint32 hold_rx, quint32 release_rx, quint32 expiries, quint32 invalid);
  void send_inhibit_status (bool supported, qint64 t_rx_ns = 0, qint64 t_pin_ns = 0);
  static bool has_our_magic (QByteArray const& datagram)
  {
    if (datagram.size () < static_cast<int> (sizeof (quint32))) return false;
    QDataStream in {datagram};
    in.setVersion (QDataStream::Qt_5_0);
    quint32 magic;
    in >> magic;
    return magic == NetworkMessage::Builder::magic;
  }
  void heartbeat ();
  void closedown ();
  StreamStatus check_status (QDataStream const&) const;
  void send_message (QByteArray const&, bool queue_if_pending = true, bool allow_duplicates = false);
  void send_message (QDataStream const& out, QByteArray const& message, bool queue_if_pending = true, bool allow_duplicates = false)
  {
    if (OK == check_status (out))
      {
        send_message (message, queue_if_pending, allow_duplicates);
      }
    else
      {
        Q_EMIT self_->error ("Error creating UDP message");
      }
  }

  MessageClient * self_;
  bool enabled_;
  QString id_;
  QString version_;
  QString revision_;
  int dns_lookup_id_;
  QHostAddress server_;
  port_type server_port_;
  int TTL_;
  std::vector<QNetworkInterface> network_interfaces_;
  quint32 schema_;
  QTimer * heartbeat_timer_;
  std::vector<QHostAddress> blocked_addresses_;
  struct InhibitSnapshot
  {
    bool valid {false};
    bool supported {false};
    bool inhibited {false};
    QString source_station;
    quint32 hold_rx {0};
    quint32 release_rx {0};
    quint32 expiries {0};
    quint32 invalid {0};
  } inhibit_status_;
  QTimer inhibit_invalid_timer_;
  quint64 inhibit_invalid_count_ {0};
  UdpDispatch dispatch_;

  // hold messages sent before host lookup completes asynchronously
  QQueue<QByteArray> pending_messages_;
  QByteArray last_message_;
};

#include "MessageClient.moc"

void MessageClient::impl::set_server (QString const& server_name, QStringList const& network_interface_names)
{
  // qDebug () << "MessageClient server:" << server_name << "port:" << server_port_ << "interfaces:" << network_interface_names;
  server_.setAddress (server_name);
  network_interfaces_.clear ();
  for (auto const& net_if_name : network_interface_names)
    {
      network_interfaces_.push_back (QNetworkInterface::interfaceFromName (net_if_name));
    }

  if (server_.isNull () && server_name.size ()) // DNS lookup required
    {
      // queue a host address lookup
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
      dns_lookup_id_ = QHostInfo::lookupHost (server_name, this, &MessageClient::impl::host_info_results);
#else
      dns_lookup_id_ = QHostInfo::lookupHost (server_name, this, SLOT (host_info_results (QHostInfo)));
#endif
    }
  else
    {
      start ();
    }
}

void MessageClient::impl::host_info_results (QHostInfo host_info)
{
  if (host_info.lookupId () != dns_lookup_id_) return;
  dns_lookup_id_ = -1;
  if (QHostInfo::NoError != host_info.error ())
    {
      Q_EMIT self_->error ("UDP server DNS lookup failed: " + host_info.errorString ());
      return;
    }
  else
    {
      auto const& server_addresses = host_info.addresses ();
      if (server_addresses.size ())
        {
          server_ = server_addresses[0];
        }
    }
  start ();
}

void MessageClient::impl::start ()
{
  if (server_.isNull ())
    {
      Q_EMIT self_->close ();
      pending_messages_.clear (); // discard
      return;
    }

  if (is_broadcast_address (server_))
    {
      Q_EMIT self_->error ("IPv4 broadcast not supported, please specify the loop-back address, a server host address, or multicast group address");
      pending_messages_.clear (); // discard
      return;
    }

  if (blocked_addresses_.end () != std::find (blocked_addresses_.begin (), blocked_addresses_.end (), server_))
    {
      Q_EMIT self_->error ("UDP server blocked, please try another");
      pending_messages_.clear (); // discard
      return;
    }

  TRACE_UDP ("Trying server:" << server_.toString ());
  QHostAddress interface_addr {QAbstractSocket::IPv6Protocol == server_.protocol ()
                               ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4};

  if (dispatch_.local_address () != interface_addr.toString ())
    {
      if (!dispatch_.is_unconnected ())
        {
          dispatch_.close_socket ();
        }
      // Ephemeral source port. Replies, including type 18, come back here.
      dispatch_.bind (interface_addr);
      dispatch_.set_ttl (TTL_);
    }

  // send initial heartbeat which allows schema negotiation
  heartbeat ();

  // clear any backlog
  while (pending_messages_.size ())
    {
      send_message (pending_messages_.dequeue (), true, false);
    }
}

void MessageClient::impl::pending_datagrams (QByteArray const& datagram, quint16 sender_port)
{
  TRACE_UDP ("message received from port:" << sender_port << "size:" << datagram.size ());
  // A UDP server that answers us replies from the socket it bound on
  // the server port, so a datagram from any other source port that
  // does not even carry our magic number is not a reply at all.
  if (sender_port != server_port_ && !has_our_magic (datagram))
    {
      TRACE_UDP ("ignored foreign datagram from port:" << sender_port << "size:" << datagram.size ());
      return;
    }
  parse_message (datagram);
}

void MessageClient::impl::invalid_inhibit ()
{
  ++inhibit_invalid_count_;
  if (!inhibit_invalid_timer_.isActive ()) inhibit_invalid_timer_.start (100);
}

bool MessageClient::impl::parse_tx_inhibit (QByteArray const& msg)
{
  if (msg.size () < 12
      || qFromBigEndian<quint32> (reinterpret_cast<uchar const *> (msg.constData () + 8))
           != NetworkMessage::TxInhibit) return false;
  if (!enabled_) return true;
  if (msg.size () < 16 || msg.size () > inhibit_message_limit
      || qFromBigEndian<quint32> (reinterpret_cast<uchar const *> (msg.constData () + 12)) > 1024
      || qFromBigEndian<quint32> (reinterpret_cast<uchar const *> (msg.constData ()))
           != NetworkMessage::Builder::magic)
    {
      invalid_inhibit ();
      return true;
    }
  try
    {
      NetworkMessage::Reader in {msg};
      if (in.status () != QDataStream::Ok || in.schema () < 2)
        {
          invalid_inhibit ();
          return true;
        }
      auto const id_length = qFromBigEndian<quint32> (reinterpret_cast<uchar const *> (msg.constData () + 12));
      if (in.id ().toUtf8 () != msg.mid (16, int (id_length)))
        {
          invalid_inhibit ();
          return true;
        }
      if (in.id () != id_) return true;
      QString controller;
      QString station;
      quint32 ttl {0};
      bool valid = read_inhibit_text (in, controller, 128, true);
      in >> ttl;
      valid = valid && in.status () == QDataStream::Ok
        && (!ttl || (ttl >= 100 && ttl <= 30000))
        && read_inhibit_text (in, station, 128, false);
      if (!valid) invalid_inhibit ();
      else Q_EMIT self_->tx_inhibit_command (controller, ttl, station);
    }
  catch (std::exception const&)
    {
      invalid_inhibit ();
    }
  return true;
}

void MessageClient::impl::parse_message (QByteArray const& msg)
{
  if (parse_tx_inhibit (msg)) return;
  try
    {
      // 
      // message format is described in NetworkMessage.hpp
      // 
      NetworkMessage::Reader in {msg};
      if (OK == check_status (in))
        {
          if (schema_ < in.schema ()) // one time record of server's
                                      // negotiated schema
            {
              schema_ = in.schema ();
            }

          if (!enabled_)
            {
              TRACE_UDP ("message processing disabled for id:" << in.id ());
              return;
            }

          //
          // message format is described in NetworkMessage.hpp
          //
          switch (in.type ())
            {
            case NetworkMessage::Reply:
              {
                // unpack message
                QTime time;
                qint32 snr;
                float delta_time;
                quint32 delta_frequency;
                QByteArray mode;
                QByteArray message;
                bool low_confidence {false};
                quint8 modifiers {0};
                in >> time >> snr >> delta_time >> delta_frequency >> mode >> message
                   >> low_confidence >> modifiers;
                TRACE_UDP ("Reply: time:" << time << "snr:" << snr << "dt:" << delta_time << "df:" << delta_frequency << "mode:" << mode << "message:" << message << "low confidence:" << low_confidence << "modifiers: 0x"
#if QT_VERSION >= QT_VERSION_CHECK (5, 15, 0)
                           << Qt::hex
#else
                           << hex
#endif
                           << modifiers);
                if (check_status (in) != Fail)
                  {
                    Q_EMIT self_->reply (time, snr, delta_time, delta_frequency
                                         , QString::fromUtf8 (mode), QString::fromUtf8 (message)
                                         , low_confidence, modifiers);
                  }
              }
              break;

            case NetworkMessage::Clear:
              {
                quint8 window {0};
                in >> window;
                TRACE_UDP ("Clear window:" << window);
                if (check_status (in) != Fail)
                  {
                    Q_EMIT self_->clear_decodes (window);
                  }
              }
              break;

            case NetworkMessage::Close:
              TRACE_UDP ("Close");
              if (check_status (in) != Fail)
                {
                  last_message_.clear ();
                  Q_EMIT self_->close ();
                }
              break;

            case NetworkMessage::Replay:
              TRACE_UDP ("Replay");
              if (check_status (in) != Fail)
                {
                  last_message_.clear ();
                  Q_EMIT self_->replay ();
                }
              break;

            case NetworkMessage::HaltTx:
              {
                bool auto_only {false};
                in >> auto_only;
                TRACE_UDP ("Halt Tx auto_only:" << auto_only);
                if (check_status (in) != Fail)
                  {
                    Q_EMIT self_->halt_tx (auto_only);
                  }
              }
              break;

            case NetworkMessage::FreeText:
              {
                QByteArray message;
                bool send {true};
                in >> message >> send;
                TRACE_UDP ("FreeText message:" << message << "send:" << send);
                if (check_status (in) != Fail)
                  {
                    Q_EMIT self_->free_text (QString::fromUtf8 (message), send);
                  }
              }
              break;

            case NetworkMessage::Location:
              {
                QByteArray location;
                in >> location;
                TRACE_UDP ("Location location:" << location);
                if (check_status (in) != Fail)
                {
                    Q_EMIT self_->location (QString::fromUtf8 (location));
                }
              }
              break;

            case NetworkMessage::HighlightCallsign:
              {
                QByteArray call;
                QColor bg;      // default invalid color
                QColor fg;      // default invalid color
                bool last_only {false};
                in >> call >> bg >> fg >> last_only;
                TRACE_UDP ("HighlightCallsign call:" << call << "bg:" << bg << "fg:" << fg << "last only:" << last_only);
                if (check_status (in) != Fail && call.size ())
                  {
                    Q_EMIT self_->highlight_callsign (QString::fromUtf8 (call), bg, fg, last_only);
                  }
              }
              break;

            case NetworkMessage::SwitchConfiguration:
              {
                QByteArray configuration_name;
                in >> configuration_name;
                TRACE_UDP ("Switch Configuration name:" << configuration_name);
                if (check_status (in) != Fail)
                  {
                    Q_EMIT self_->switch_configuration (QString::fromUtf8 (configuration_name));
                  }
              }
              break;

            case NetworkMessage::Configure:
              {
                QByteArray mode;
                quint32 frequency_tolerance;
                QByteArray submode;
                bool fast_mode {false};
                quint32 tr_period {std::numeric_limits<quint32>::max ()};
                quint32 rx_df {std::numeric_limits<quint32>::max ()};
                QByteArray dx_call;
                QByteArray dx_grid;
                bool generate_messages {false};
                in >> mode >> frequency_tolerance >> submode >> fast_mode >> tr_period >> rx_df
                   >> dx_call >> dx_grid >> generate_messages;
                TRACE_UDP ("Configure mode:" << mode << "frequency tolerance:" << frequency_tolerance << "submode:" << submode << "fast mode:" << fast_mode << "T/R period:" << tr_period << "rx df:" << rx_df << "dx call:" << dx_call << "dx grid:" << dx_grid << "generate messages:" << generate_messages);
                if (check_status (in) != Fail)
                  {
                    Q_EMIT self_->configure (QString::fromUtf8 (mode), frequency_tolerance
                                             , QString::fromUtf8 (submode), fast_mode, tr_period, rx_df
                                             , QString::fromUtf8 (dx_call), QString::fromUtf8 (dx_grid)
                                             , generate_messages);
                  }
              }
              break;

            case NetworkMessage::AnnotationInfo: {
              QByteArray dx_call;
              bool sort_order_provided{false};
              quint32 sort_order{std::numeric_limits<quint32>::max()};
              in >> dx_call >> sort_order_provided >> sort_order;
              TRACE_UDP ("External Callsign Info:" << dx_call << "sort_order_provided:" << sort_order_provided
                                                  << "sort_order:" << sort_order);
              if (sort_order > 50000) sort_order = 50000;
              if (check_status(in) != Fail) {
                Q_EMIT
                  self_->annotation_info(QString::fromUtf8(dx_call), sort_order_provided, sort_order);
              }
            }
            break;

            default:
              // Ignore
              //
              // Note that although server  heartbeat messages are not
              // parsed here  they are  still partially parsed  in the
              // message reader class to  negotiate the maximum schema
              // number being used on the network.
              if (NetworkMessage::Heartbeat != in.type ())
                {
                  TRACE_UDP ("ignoring message type:" << in.type ());
                }
              break;
            }
        }
      else
        {
          TRACE_UDP ("ignored message for id:" << in.id ());
        }
    }
  catch (std::exception const& e)
    {
      Q_EMIT self_->error (QString {"MessageClient exception: %1"}.arg (e.what ()));
    }
  catch (...)
    {
      Q_EMIT self_->error ("Unexpected exception in MessageClient");
    }
}

void MessageClient::impl::heartbeat ()
{
   if (server_port_ && !server_.isNull ())
    {
      QByteArray message;
      NetworkMessage::Builder out {&message, NetworkMessage::Heartbeat, id_, schema_};
      out << NetworkMessage::Builder::schema_number // maximum schema number accepted
          << version_.toUtf8 () << revision_.toUtf8 ();
      TRACE_UDP ("schema:" << schema_ << "max schema:" << NetworkMessage::Builder::schema_number << "version:" << version_ << "revision:" << revision_);
      send_message (out, message, false, true);
      if (inhibit_available ()) send_inhibit_status (true);
    }
}

bool MessageClient::impl::inhibit_available () const
{
  return enabled_ && inhibit_status_.valid && inhibit_status_.supported;
}

void MessageClient::impl::update_inhibit_status (
  bool supported, bool inhibited, QString const& source_station,
  quint32 hold_rx, quint32 release_rx, quint32 expiries, quint32 invalid)
{
  auto const was_available = inhibit_available ();
  qint64 t_rx_ns = 0;
  qint64 t_pin_ns = 0;
  // The pair belongs to the edge that reports a new hold. A refresh
  // and a heartbeat send zeros.
  if (inhibited && !inhibit_status_.inhibited)
    {
      TxInhibitDrop::take_pin_stamps (t_rx_ns, t_pin_ns);
    }
  inhibit_status_.valid = true;
  inhibit_status_.supported = supported;
  inhibit_status_.inhibited = inhibited;
  inhibit_status_.source_station = source_station;
  inhibit_status_.hold_rx = hold_rx;
  inhibit_status_.release_rx = release_rx;
  inhibit_status_.expiries = expiries;
  inhibit_status_.invalid = invalid;
  if (inhibit_available ()) send_inhibit_status (true, t_rx_ns, t_pin_ns);
  else if (was_available) send_inhibit_status (false);
}

void MessageClient::impl::send_inhibit_status (bool supported, qint64 t_rx_ns, qint64 t_pin_ns)
{
  if (!inhibit_status_.valid || !server_port_ || server_.isNull ()) return;
  QByteArray message;
  NetworkMessage::Builder out {&message, NetworkMessage::InhibitStatus, id_, schema_};
  out << supported << inhibit_status_.inhibited
      << inhibit_status_.source_station.toUtf8 ()
      << inhibit_status_.hold_rx << inhibit_status_.release_rx
      << inhibit_status_.expiries << inhibit_status_.invalid
      << static_cast<quint64> (t_rx_ns) << static_cast<quint64> (t_pin_ns);
  TRACE_UDP ("supported:" << supported << "inhibited:" << inhibit_status_.inhibited
             << "source:" << inhibit_status_.source_station);
  send_message (out, message, false, true);
}

void MessageClient::impl::closedown ()
{
   if (server_port_ && !server_.isNull ())
    {
      QByteArray message;
      NetworkMessage::Builder out {&message, NetworkMessage::Close, id_, schema_};
      TRACE_UDP ("");
      send_message (out, message, false);
    }
}

void MessageClient::impl::send_message (QByteArray const& message, bool queue_if_pending, bool allow_duplicates)
{
  if (server_port_)
    {
      if (!server_.isNull ())
        {
          if (allow_duplicates || message != last_message_) // avoid duplicates
            {
              if (is_multicast_address (server_))
                {
                  std::for_each (network_interfaces_.begin (), network_interfaces_.end ()
                                 , [&] (QNetworkInterface const& net_if) {
                                     dispatch_.send_one (message, server_, server_port_, net_if.name ());
                                   });
                }
              else
                {
                  dispatch_.send_one (message, server_, server_port_, QString {});
                }
              last_message_ = message;
            }
        }
      else if (queue_if_pending)
        {
          pending_messages_.enqueue (message);
        }
    }
}

auto MessageClient::impl::check_status (QDataStream const& stream) const -> StreamStatus
{
  auto stat = stream.status ();
  StreamStatus result {Fail};
  switch (stat)
    {
    case QDataStream::ReadPastEnd:
      result = Short;
      break;

    case QDataStream::ReadCorruptData:
      Q_EMIT self_->error ("Message serialization error: read corrupt data");
      break;

    case QDataStream::WriteFailed:
      Q_EMIT self_->error ("Message serialization error: write error");
      break;

    default:
      result = OK;
      break;
    }
  return result;
}

MessageClient::MessageClient (QString const& id, QString const& version, QString const& revision,
                              QString const& server_name, port_type server_port,
                              QStringList const& network_interface_names,
                              int TTL, QObject * self)
  : QObject {self}
  , m_ {id, version, revision, server_port, TTL, this}
{
  m_->set_server (server_name, network_interface_names);
}

QHostAddress MessageClient::server_address () const
{
  return m_->server_;
}

auto MessageClient::server_port () const -> port_type
{
  return m_->server_port_;
}

void MessageClient::set_server (QString const& server_name, QStringList const& network_interface_names)
{
  m_->set_server (server_name, network_interface_names);
}

void MessageClient::set_server_port (port_type server_port)
{
  m_->server_port_ = server_port;
}

void MessageClient::set_TTL (int TTL)
{
  m_->TTL_ = TTL;
  m_->dispatch_.set_ttl (m_->TTL_);
}

void MessageClient::enable (bool flag)
{
  if (m_->enabled_ == flag) return;
  auto const was_available = m_->inhibit_available ();
  m_->enabled_ = flag;
  m_->dispatch_.set_commands_enabled (flag);
  if (m_->inhibit_available ()) m_->send_inhibit_status (true);
  else if (was_available) m_->send_inhibit_status (false);
}

void MessageClient::set_id (QString const& id)
{
  if (id != m_->id_)
    {
      // servers tell clients apart by id, so sign off under the old
      // id and announce the new one straight away
      m_->closedown ();
      m_->id_ = id;
      m_->dispatch_.set_id (id);
      m_->heartbeat ();
    }
}

void MessageClient::status_update (Frequency f, QString const& mode, QString const& dx_call
                                   , QString const& report, QString const& tx_mode
                                   , bool tx_enabled, bool transmitting, bool decoding
                                   , quint32 rx_df, quint32 tx_df, QString const& de_call
                                   , QString const& de_grid, QString const& dx_grid
                                   , bool watchdog_timeout, QString const& sub_mode
                                   , bool fast_mode, quint8 special_op_mode
                                   , quint32 frequency_tolerance, quint32 tr_period
                                   , QString const& configuration_name
                                   , QString const& tx_message
                                   , QString const& tx_symbols)
{
  if (m_->server_port_ && !m_->server_.isNull ())
    {
      QByteArray message;
      NetworkMessage::Builder out {&message, NetworkMessage::Status, m_->id_, m_->schema_};
      out << f << mode.toUtf8 () << dx_call.toUtf8 () << report.toUtf8 () << tx_mode.toUtf8 ()
          << tx_enabled << transmitting << decoding << rx_df << tx_df << de_call.toUtf8 ()
          << de_grid.toUtf8 () << dx_grid.toUtf8 () << watchdog_timeout << sub_mode.toUtf8 ()
          << fast_mode << special_op_mode << frequency_tolerance << tr_period << configuration_name.toUtf8 ()
          << tx_message.toUtf8 () << tx_symbols.toUtf8 ();
      TRACE_UDP ("frequency:" << f << "mode:" << mode << "DX:" << dx_call << "report:" << report << "Tx mode:" << tx_mode << "tx_enabled:" << tx_enabled << "Tx:" << transmitting << "decoding:" << decoding << "Rx df:" << rx_df << "Tx df:" << tx_df << "DE:" << de_call << "DE grid:" << de_grid << "DX grid:" << dx_grid << "w/d t/o:" << watchdog_timeout << "sub_mode:" << sub_mode << "fast mode:" << fast_mode << "spec op mode:" << special_op_mode << "frequency tolerance:" << frequency_tolerance << "T/R period:" << tr_period << "configuration name:" << configuration_name << "Tx message:" << tx_message << "Tx symbols:" << tx_symbols);
      m_->send_message (out, message);
    }
}

void MessageClient::decode (bool is_new, QTime time, qint32 snr, float delta_time, quint32 delta_frequency
                            , QString const& mode, QString const& message_text, bool low_confidence
                            , bool off_air)
{
   if (m_->server_port_ && !m_->server_.isNull ())
    {
      QByteArray message;
      NetworkMessage::Builder out {&message, NetworkMessage::Decode, m_->id_, m_->schema_};
      out << is_new << time << snr << delta_time << delta_frequency << mode.toUtf8 ()
          << message_text.toUtf8 () << low_confidence << off_air;
      TRACE_UDP ("new" << is_new << "time:" << time << "snr:" << snr << "dt:" << delta_time << "df:" << delta_frequency << "mode:" << mode << "text:" << message_text << "low conf:" << low_confidence << "off air:" << off_air);
      m_->send_message (out, message);
    }
}

void MessageClient::inhibit_status (bool supported, bool inhibited
                                    , QString const& source_station
                                    , quint32 hold_rx, quint32 release_rx
                                    , quint32 expiries, quint32 invalid)
{
  m_->update_inhibit_status (supported, inhibited, source_station,
                             hold_rx, release_rx, expiries, invalid);
}

void MessageClient::WSPR_decode (bool is_new, QTime time, qint32 snr, float delta_time, Frequency frequency
                                 , qint32 drift, QString const& callsign, QString const& grid, qint32 power
                                 , bool off_air)
{
   if (m_->server_port_ && !m_->server_.isNull ())
    {
      QByteArray message;
      NetworkMessage::Builder out {&message, NetworkMessage::WSPRDecode, m_->id_, m_->schema_};
      out << is_new << time << snr << delta_time << frequency << drift << callsign.toUtf8 ()
          << grid.toUtf8 () << power << off_air;
      TRACE_UDP ("new:" << is_new << "time:" << time << "snr:" << snr << "dt:" << delta_time << "frequency:" << frequency << "drift:" << drift << "call:" << callsign << "grid:" << grid << "pwr:" << power << "off air:" << off_air);
      m_->send_message (out, message);
    }
}

void MessageClient::decodes_cleared ()
{
   if (m_->server_port_ && !m_->server_.isNull ())
    {
      QByteArray message;
      NetworkMessage::Builder out {&message, NetworkMessage::Clear, m_->id_, m_->schema_};
      TRACE_UDP ("");
      m_->send_message (out, message);
    }
}

void MessageClient::qso_logged (QDateTime time_off, QString const& dx_call, QString const& dx_grid
                                , Frequency dial_frequency, QString const& mode, QString const& report_sent
                                , QString const& report_received, QString const& tx_power
                                , QString const& comments, QString const& name, QDateTime time_on
                                , QString const& operator_call, QString const& my_call
                                , QString const& my_grid, QString const& exchange_sent
                                , QString const& exchange_rcvd, QString const& propmode
                                , QString const& satellite, QString const& satmode, QString const& freqRx)
{
   if (m_->server_port_ && !m_->server_.isNull ())
    {
      QByteArray message;
      NetworkMessage::Builder out {&message, NetworkMessage::QSOLogged, m_->id_, m_->schema_};
      out << time_off << dx_call.toUtf8 () << dx_grid.toUtf8 () << dial_frequency << mode.toUtf8 ()
          << report_sent.toUtf8 () << report_received.toUtf8 () << tx_power.toUtf8 () << comments.toUtf8 ()
          << name.toUtf8 () << time_on << operator_call.toUtf8 () << my_call.toUtf8 () << my_grid.toUtf8 ()
          << exchange_sent.toUtf8 () << exchange_rcvd.toUtf8 () << propmode.toUtf8 () << satellite.toUtf8 ()
          << satmode.toUtf8 () << freqRx.toUtf8 ();
      TRACE_UDP ("time off:" << time_off << "DX:" << dx_call << "DX grid:" << dx_grid << "dial:" << dial_frequency << "mode:" << mode << "sent:" << report_sent << "rcvd:" << report_received << "pwr:" << tx_power << "comments:" << comments << "name:" << name << "time on:" << time_on << "op:" << operator_call << "DE:" << my_call << "DE grid:" << my_grid << "exch sent:" << exchange_sent << "exch rcvd:" << exchange_rcvd  << "prop_mode:" << propmode << "sat_name:" << satellite << "sat_mode:" << satmode << "freq_rx:" << freqRx);
      m_->send_message (out, message);
    }
}

void MessageClient::logged_ADIF (QByteArray const& ADIF_record)
{
   if (m_->server_port_ && !m_->server_.isNull ())
    {
      QByteArray message;
      NetworkMessage::Builder out {&message, NetworkMessage::LoggedADIF, m_->id_, m_->schema_};
      QByteArray ADIF {"\n<adif_ver:5>3.1.0\n<programid:6>WSJT-X\n<EOH>\n" + ADIF_record + " <EOR>"};
      out << ADIF;
      TRACE_UDP ("ADIF:" << ADIF);
      m_->send_message (out, message);
    }
}
