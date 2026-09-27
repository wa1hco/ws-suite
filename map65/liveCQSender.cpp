#include "liveCQSender.hpp"
#include <QSettings>
#include "SettingsGroup.hpp"
#include "mainwindow.h"
#include "qt_helpers.hpp"
#include "../revision_utils.hpp"

#include <fstream>
#include <iostream>
#include <cmath>
#include <QObject>
#include <QString>
#include <QDateTime>
#include <QSharedPointer>
#include <QSslSocket>
#include <QHostInfo>
#include <QQueue>
#include <QByteArray>
#include <QDataStream>
#include <QTimer>
#include <QDir>
#include <QSettings>
#include <QDebug>
#include <QMetaType>
#include <QUrl>
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
#include <QRandomGenerator>
#endif

#include "devsetup.h"

# define DEBUGPSK 1;

// The service port and transport (TLS or plain TCP) now follow the URL:
// https://... -> TLS, default port 443; http://... -> plain, default port 80;
// an explicit port (http://localhost:3000/...) overrides either default, so
// a local dev server works as the "Other" destination.

liveCQSender::liveCQSender(QString const& myCall,
                           QString const& myGrid,
                           QString const& theUrl)
    :
      m_myCall{myCall},
      m_myGrid{myGrid},
      m_theUrl{theUrl}
{
}
    
  void liveCQSender::init() {
    // qDebug() << "liveCQSender::init running on thread: " << QThread::currentThread();      
    socket.reset(new QSslSocket(this));
    
    // Connect signals before connecting
    connect(socket.data(), &QAbstractSocket::connected, this, &liveCQSender::onConnected);
    connect(socket.data(), &QAbstractSocket::readyRead, this, &liveCQSender::onReadyRead); 
    connect(this, &liveCQSender::dataReady, this, &liveCQSender::sendData);   
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(socket.data(), &QAbstractSocket::errorOccurred, this, &liveCQSender::handle_socket_error);  
#else   
    connect(socket.data(), static_cast<void(QAbstractSocket::*)(QAbstractSocket::SocketError)>(&QAbstractSocket::error),
        this, &liveCQSender::handle_socket_error);  
#endif
    QString host, path;
    quint16 port;
    bool tls;
    if (!parseUrl(host, port, path, tls)) return;
    // Start connection (non-blocking)
    connectSocket(host, port, tls);
  }

  bool liveCQSender::parseUrl (QString& host, quint16& port, QString& path, bool& tls) const {
    if (m_theUrl.startsWith("https://")) tls = true;
    else if (m_theUrl.startsWith("http://")) tls = false;
    else return false;
    QUrl parsed(m_theUrl);
    host = parsed.host();
    if (host.isEmpty()) return false;
    port = quint16(parsed.port(tls ? 443 : 80));
    path = parsed.path();
    if (path.isEmpty()) path = "/";
    return true;
  }

  void liveCQSender::connectSocket (QString const& host, quint16 port, bool tls) {
    if (tls) socket->connectToHostEncrypted(host, port);
    else socket->connectToHost(host, port);
  }
  
  void liveCQSender::onConnected() {
    
    qDebug() << "LiveCQSender onConnected Connected securely!";
   
    for(const QByteArray &ba : onConnectedRequests) {
      qDebug() << "liveCQSender::onConnected request: " << ba;
      socket->write(ba); 
    }
    onConnectedRequests.clear();
  }  
  
  void liveCQSender::onReadyRead() {
    QByteArray data = socket->readAll();
    // Parse headers, then handle chunks
    while (!data.isEmpty()) {
        int pos = data.indexOf("\r\n");
        bool ok;
        int size = data.left(pos).trimmed().toInt(&ok, 16);
        if (!ok || size == 0) break;
        data = data.mid(pos + 2);
        qDebug() << "Chunk:" << data.left(size);
        data = data.mid(size + 2); // skip \r\n
    }
      qDebug() << "liveCQSender Response:" <<   data;
}   
   
  void liveCQSender::handle_socket_error(QAbstractSocket::SocketError error)
  {
    qDebug() << "liveCQSender Socket error:" << error << socket->errorString();
  }   
  
  void liveCQSender::addRemoteStation (QByteArray const& postByteArray, QString const& theUrl)
  {
    qDebug() << theUrl;
    // Adopt the caller's current URL so a settings change (or the new
    // N6NU/Other destinations) takes effect without recreating the sender.
    // This slot runs in the sender's own thread (queued connection), so
    // touching m_theUrl here is safe.
    if (!theUrl.isEmpty() && theUrl != m_theUrl) m_theUrl = theUrl;
    emit dataReady(postByteArray);
  }
  
  void liveCQSender::sendData(const QByteArray &payload1) {
    QByteArray request;
    QByteArray body;
    QString host, webpage;
    quint16 port;
    bool tls;
    if(!payload1.isEmpty())
    {
      body.append(payload1); // Your data here
    }
    qDebug() << "m_theUrl is: " << m_theUrl;
    if (!parseUrl(host, port, webpage, tls)) return;
    request.append(("POST " + webpage + " HTTP/1.1\r\n").toUtf8());
    QString hostHdr = host;
    if (port != (tls ? 443 : 80)) hostHdr += ":" + QString::number(port);
    request.append(("Host: " + hostHdr + "\r\n").toUtf8());
    request.append("Content-Type: application/x-www-form-urlencoded\r\n");
    request.append("Connection: keep-alive\r\n");
    request.append("Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n");
    if(!payload1.isEmpty())
    {
      request.append(body);
    }
    qDebug() << "LiveCQ sendData request: " << request;
    if (socket->state() == QAbstractSocket::ConnectedState) {
      socket->write(request);  
      qDebug() << "liveCQSender::sendData request2: " << request;
      }
    else {
      onConnectedRequests.append(request);
      request.clear();
      connectSocket(host, port, tls);
      // Data will be sent in onConnected()
    }
  }
  
  liveCQSender::~liveCQSender() = default; // Define it, even as default   
   
