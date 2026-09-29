#include <QtTest>

#include "TxInhibit/TxInhibitDrop.hpp"

// Records each change of the combined pin. Same rule as the inhibit thread:
// pin = ptt_intent AND NOT inhibit, one write per change.
class PinTrace
{
public:
  void set_intent (bool on) { intent_ = on; apply (); }
  void set_inhibit (bool on) { inhibit_ = on; apply (); }
  bool pin () const { return pin_; }
  int writes () const { return writes_; }

private:
  void apply ()
  {
    bool const next = TxInhibitDrop::pin_level (intent_, inhibit_);
    if (writes_ != 0 && next == pin_) return;
    pin_ = next;
    ++writes_;
  }
  bool intent_ {false};
  bool inhibit_ {false};
  bool pin_ {false};
  int writes_ {0};
};

class TestPinCombiner final : public QObject
{
  Q_OBJECT

private slots:
  void level_ ();
  void six_edges_ ();
};

void TestPinCombiner::level_ ()
{
  QVERIFY (!TxInhibitDrop::pin_level (false, false));
  QVERIFY (TxInhibitDrop::pin_level (true, false));
  QVERIFY (!TxInhibitDrop::pin_level (false, true));
  QVERIFY (!TxInhibitDrop::pin_level (true, true));
}

void TestPinCombiner::six_edges_ ()
{
  PinTrace pin;
  QCOMPARE (pin.writes (), 0);

  pin.set_intent (true);
  QVERIFY (pin.pin ());
  QCOMPARE (pin.writes (), 1);

  pin.set_inhibit (true);
  QVERIFY (!pin.pin ());
  QCOMPARE (pin.writes (), 2);

  pin.set_intent (false);
  QVERIFY (!pin.pin ());
  QCOMPARE (pin.writes (), 2);

  pin.set_intent (true);
  QVERIFY (!pin.pin ());
  QCOMPARE (pin.writes (), 2);

  pin.set_inhibit (false);
  QVERIFY (pin.pin ());
  QCOMPARE (pin.writes (), 3);

  pin.set_inhibit (true);
  QVERIFY (!pin.pin ());
  QCOMPARE (pin.writes (), 4);
  pin.set_inhibit (false);
  QVERIFY (pin.pin ());
  QCOMPARE (pin.writes (), 5);
  pin.set_intent (false);
  QVERIFY (!pin.pin ());
  QCOMPARE (pin.writes (), 6);
}

QTEST_MAIN (TestPinCombiner)
#include "test_pin_combiner.moc"
