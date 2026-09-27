#ifndef TX_INHIBIT_GATE_HPP__
#define TX_INHIBIT_GATE_HPP__

// ---------------------------------------------------------------------------
// TxInhibit module — UDP listen + want_tx mix for RTS/DTR PTT
//
//   assert PTT  ⇔  want_tx  and  not hold
//
// • want_tx arrives from HamlibTransceiver::do_ptt() via set_intent().
// • Hold is private (UDP keepalives + hold_timeout_ms safety timeout).
//   CW anti-chatter hang lives only in the KEY agent; normal end is
//   release hold (ttl_ms: 0). See docs/TX_INHIBIT.md §3.
// • The UDP socket runs on its own thread and clears RTS or DTR itself when
//   a hold arrives. This object, on the transceiver thread, still decides
//   want_tx and asks Hamlib to update its PTT cache.
// • WSJT-X station = this WSJT-X station (app + PC + radio + antenna).
//
// Child of HamlibTransceiver. The listen thread is not the transceiver thread.
//
// Design authority / glossary: docs/TX_INHIBIT.md
// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>

class QTimer;

class TxInhibitGate
  : public QObject
{
  Q_OBJECT

public:
  explicit TxInhibitGate (QObject * parent = nullptr);
  ~TxInhibitGate () override;

  static int constexpr maximum_tracked_holds {64};

public slots:
  // Start the hold-expiry timer. Commands arrive as type 18 on the
  // reporting UDP socket, not on a private port.
  void start_listening ();

  // Type 18. ttl 0 releases this controller. ttl 100..30000 holds.
  void command (QString controller, quint32 ttl_ms, QString station);
  void note_invalid (quint64 count);

  // WSJT-X TX intent from do_ptt(on). Physical line is driven via physicalPtt.
  void set_intent (bool on);

  // Force intent off, stop UDP/timer (rig close / shutdown).
  // If emit_pin is false, do not request a physical PTT change (caller already
  // closed Hamlib or will set the pin itself). Safe to call more than once.
  void shutdown (bool emit_pin = true);

signals:
  // Ask HamlibTransceiver to call rig_set_ptt (radiate true/false).
  // Same thread (DirectConnection) when parent is HamlibTransceiver.
  void physicalPtt (bool radiate);

  // Queued to GUI: status-bar badge + optional InhibitStatus counters.
  void inhibitChanged (bool inhibited, QString const& source
                       , quint32 hold_rx, quint32 release_rx
                       , quint32 expiries, quint32 invalid);

  // Non-fatal operator-visible problems.
  // Callers must not treat this as a rig CAT/PTT failure.
  void lineError (QString const& message);

  // rig_set_ptt failed while the gate was driving the pin. Unlike lineError
  // (bind problems), this should surface as a rig/operator failure so the UI
  // does not show "Tx" with no RF and no message.
  void pttApplyFailed (QString const& message);

private slots:
  void tick ();

private:
  struct Hold
  {
    qint64 expires_at;
    QString holder;
  };

  void sweep (qint64 now);
  QString holder_summary () const;
  void apply_line ();
  // Emits physicalPtt with exceptions contained. The slot on the other end
  // calls rig_set_ptt, which throws; this is reached from timer and socket
  // slots where an escaping exception would abort the application.
  void emit_physical_ptt (bool radiate);
  void emit_state_if_changed ();
  qint64 now_ms () const;

  // Monotonic time base for hold expiry: immune to system-clock steps, which
  // WSJT-X hosts take routinely from time-sync tools. See now_ms().
  QElapsedTimer uptime_;
  QHash<QString, Hold> holds_;
  QTimer * timer_ {nullptr};
  bool intent_ {false};
  bool last_radiate_ {false};
  bool last_emitted_inhibited_ {false};
  bool stopped_ {false};         // after shutdown: no further pin emits
  QString last_badge_;
  quint32 hold_rx_ {0};
  quint32 release_rx_ {0};
  quint32 expiries_ {0};
  quint32 invalid_ {0};
};

#endif
