#include "TxInhibitGate.hpp"
#include "TxInhibitDrop.hpp"

#include <algorithm>
#include <exception>

#include <QSet>
#include <QStringList>
#include <QTimer>

TxInhibitGate::TxInhibitGate (QObject * parent)
  : QObject {parent}
{
  uptime_.start ();
}

TxInhibitGate::~TxInhibitGate ()
{
  // Never emit physicalPtt from the destructor — Hamlib may already be closed.
  shutdown (false);
}

qint64 TxInhibitGate::now_ms () const
{
  // MONOTONIC, deliberately. Hold expiry compares against an absolute value in
  // this same time base, so a wall clock would let a clock *step* corrupt it:
  //
  //   step backwards  -> the hold outlives its timeout by the step size. PTT
  //                      stays off with no packet to explain it.
  //   step forwards   -> the hold ends early. PTT can assert while the priority
  //                      station is still keyed -- the exact failure this
  //                      feature exists to prevent.
  //
  // Not hypothetical for this audience: WSJT-X operators run Meinberg NTP,
  // Dimension4, BktTimeSync and similar, all of which step the system clock,
  // often repeatedly. QElapsedTimer is unaffected by clock changes.
  // See docs/REVIEW-rc2.md C1.
  return uptime_.isValid () ? uptime_.elapsed () : 0;
}

void TxInhibitGate::start_listening ()
{
  stopped_ = false;
  if (!timer_)
    {
      timer_ = new QTimer (this);
      timer_->setInterval (20);
      QObject::connect (timer_, &QTimer::timeout, this, &TxInhibitGate::tick);
      timer_->start ();
    }
  // Advertise Inhibited false so type 17 can start with the heartbeats.
  last_emitted_inhibited_ = true;
  emit_state_if_changed ();
}

void TxInhibitGate::set_intent (bool on)
{
  if (stopped_)
    {
      return;
    }
  intent_ = on;
  apply_line ();
}

void TxInhibitGate::shutdown (bool emit_pin)
{
  stopped_ = true;
  intent_ = false;

  if (emit_pin && last_radiate_)
    {
      // Request pin low once while Hamlib is still open (caller responsibility).
      last_radiate_ = false;
      emit_physical_ptt (false);   // teardown must not throw
    }
  else
    {
      last_radiate_ = false;
    }

  if (timer_)
    {
      timer_->stop ();
      timer_->deleteLater ();
      timer_ = nullptr;
    }
  holds_.clear ();
}

namespace
{
  QString const overflow_hold_key;

  QString sanitize_holder (QString const& value)
  {
    QString result;
    result.reserve (std::min (value.size (), 64));
    for (auto const character : value.left (64))
      {
        if (character.isPrint ()) result.append (character);
      }
    return result.trimmed ();
  }
}

void TxInhibitGate::command (QString controller, quint32 ttl_ms, QString station)
{
  if (stopped_) return;
  auto const now = now_ms ();
  sweep (now);
  if (!ttl_ms)
    {
      ++release_rx_;
      holds_.remove (controller);
    }
  else if (ttl_ms < 100 || ttl_ms > 30000)
    {
      ++invalid_;
    }
  else
    {
      ++hold_rx_;
      auto const expires_at = now + ttl_ms;
      auto const holder = sanitize_holder (station.isEmpty () ? controller : station);
      auto existing = holds_.find (controller);
      if (existing != holds_.end ())
        {
          *existing = Hold {expires_at, holder};
        }
      else
        {
          auto const tracked = holds_.size () - (holds_.contains (overflow_hold_key) ? 1 : 0);
          if (tracked < maximum_tracked_holds)
            {
              holds_.insert (controller, Hold {expires_at, holder});
            }
          else
            {
              auto overflow = holds_.find (overflow_hold_key);
              if (overflow == holds_.end ())
                {
                  holds_.insert (overflow_hold_key, Hold {expires_at, {}});
                }
              else
                {
                  overflow->expires_at = std::max (overflow->expires_at, expires_at);
                }
            }
        }
    }
  apply_line ();
  emit_state_if_changed ();
}

void TxInhibitGate::note_invalid (quint64 count)
{
  if (stopped_ || !count) return;
  invalid_ += static_cast<quint32> (count);
  emit_state_if_changed ();
}

void TxInhibitGate::sweep (qint64 now)
{
  for (auto it = holds_.begin (); it != holds_.end ();)
    {
      if (it->expires_at <= now)
        {
          it = holds_.erase (it);
          ++expiries_;
        }
      else
        {
          ++it;
        }
    }
}

QString TxInhibitGate::holder_summary () const
{
  QStringList holders;
  QSet<QString> seen;
  for (auto it = holds_.cbegin (); it != holds_.cend (); ++it)
    {
      if (!it->holder.isEmpty () && !seen.contains (it->holder))
        {
          seen.insert (it->holder);
          holders.append (it->holder);
        }
    }
  holders.sort (Qt::CaseInsensitive);
  return holders.join (QStringLiteral (", "));
}

void TxInhibitGate::tick ()
{
  if (stopped_)
    {
      return;
    }
  sweep (now_ms ());
  apply_line ();
  emit_state_if_changed ();
}

void TxInhibitGate::apply_line ()
{
  if (stopped_)
    {
      return;
    }
  sweep (now_ms ());
  // Sole policy: assert PTT ⇔ want_tx and not hold.
  bool const radiate = intent_ && holds_.isEmpty ();
  // While a hold is active the modem line is cleared here, not only when
  // WS thinks it is transmitting. A stuck RTS would otherwise stay high.
  if (!radiate && !holds_.isEmpty ()) TxInhibitDrop::drop_direct ();
  if (radiate == last_radiate_)
    {
      return;
    }
  last_radiate_ = radiate;
  emit_physical_ptt (radiate);
}

// physicalPtt is a DirectConnection into HamlibTransceiver::apply_physical_ptt,
// which calls rig_set_ptt and *throws* on any Hamlib error. This is reached from
// tick() (a QTimer slot) and on_udp_ready() (a readyRead slot), so an escaping
// exception would unwind through QMetaObject::activate into the transceiver
// thread's event loop; ExceptionCatchingApplication::notify catches it and calls
// qFatal, killing the application.
//
// The stock path does not have this exposure: do_ptt is called from
// TransceiverBase::set, which already wraps everything in try/catch. Only the
// gate-driven path is unguarded, so the catch belongs here rather than inside
// apply_physical_ptt -- putting it there would also swallow errors on the stock
// path, where they are supposed to propagate and fail the rig.
//
// Realistic trigger: serial adapter unplugged or radio powered off while a hold
// is active and want_tx is true. The hold expires, the gate tries to assert PTT,
// rig_set_ptt fails, and the application dies.
//
// last_radiate_ is deliberately NOT rolled back on failure. The pin state is
// unknown after a failed set, and rolling back would make the 50 Hz tick retry
// forever, turning one dead cable into an error storm. The rig's own polling
// will notice and fail the transceiver properly.
void TxInhibitGate::emit_physical_ptt (bool radiate)
{
  try
    {
      Q_EMIT physicalPtt (radiate);
    }
  catch (std::exception const& e)
    {
      // Contain the exception (timer/socket slots must not qFatal) but do not
      // hide the failure: pttApplyFailed is wired to Transceiver::failure.
      Q_EMIT pttApplyFailed (QStringLiteral ("TX Inhibit: setting PTT %1 failed: %2")
                             .arg (radiate ? QStringLiteral ("on") : QStringLiteral ("off"))
                             .arg (QString::fromUtf8 (e.what ())));
    }
  catch (...)
    {
      Q_EMIT pttApplyFailed (QStringLiteral ("TX Inhibit: setting PTT %1 failed (unknown error)")
                             .arg (radiate ? QStringLiteral ("on") : QStringLiteral ("off")));
    }
}

void TxInhibitGate::emit_state_if_changed ()
{
  bool const inh = !holds_.isEmpty ();
  auto const badge = holder_summary ();
  if (inh != last_emitted_inhibited_ || badge != last_badge_)
    {
      last_emitted_inhibited_ = inh;
      last_badge_ = badge;
      Q_EMIT inhibitChanged (inh, badge, hold_rx_, release_rx_, expiries_, invalid_);
    }
}
