#ifndef REBRANDING_MIGRATION_HPP__
#define REBRANDING_MIGRATION_HPP__

// One-time carry-over of settings and user files from the programs' old names
// (WSJT-X, QMAP, MAP65) to the new ones (WS, WSMAP, EME65).
//
// Header-only because WS, WSMAP and EME65 are separate executables that each
// compile their own sources; a header needs no build-system change in any of
// them.
//
// The rules, and why each exists:
//
// - Files are COPIED, never moved. The old program keeps working, and the two
//   are expected to sit side by side on one machine. Copying wholesale rather
//   than walking keys also preserves the binary QVariant values WSJT-X stores
//   for PTTMethod and SplitMode.
//
// - The migration runs once per new settings file, and a marker file
//   "<settings file>.migrated" beside it records that it ran. After that
//   nothing is copied again, so a user who later deletes a log in the new
//   program does not find the old one silently back.
//
// - A file that already exists at the destination when the migration runs is
//   renamed to "<name>.pre-migration-<UTC timestamp>" and the legacy file is
//   copied in its place. Every build that contains this code writes the marker
//   on its very first start, so such a file can only come from a pre-release
//   build of the renamed program - which is exactly what testers had, and why
//   their real settings never arrived. Nothing is deleted; the old file stays
//   next to the new one.
//
// - The marker is created BEFORE anything is copied. If it cannot be written,
//   nothing is copied: a migration that could not record itself would repeat
//   on every start and keep replacing the user's new settings.
//
// - Files added to the carry-over later (run_additional_files) get their own
//   marker, "<settings file>.migrated-files", so users who already went
//   through the first migration get them too. That pass never replaces
//   anything: by then the user may have edited or downloaded the file in the
//   new program, so it copies only where the file is missing.

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTextStream>

namespace rebranding_migration
{
  struct Item
  {
    Item (QString const& from_path, QString const& to_path, bool only_if_non_empty = false)
      : from {from_path}
      , to {to_path}
      , non_empty {only_if_non_empty}
    {
    }

    QString from;       // file of the old program; empty or missing = nothing to copy
    QString to;         // where the new program expects it
    bool non_empty;     // a zero-byte file counts as absent on both sides (CALL3.TXT)
  };

  inline QString marker_for (QString const& settings_file)
  {
    return settings_file + QStringLiteral (".migrated");
  }

  inline QString files_marker_for (QString const& settings_file)
  {
    return settings_file + QStringLiteral (".migrated-files");
  }

  // true once this settings file has been through the migration
  inline bool done (QString const& settings_file)
  {
    return QFile::exists (marker_for (settings_file));
  }

  // Copy one item. Returns the line recorded in the marker file; sets copied.
  // With replace false a file already at the destination is kept.
  inline QString copy_item (Item const& item, bool& copied, bool replace = true)
  {
    copied = false;
    auto const native = [] (QString const& p) { return QDir::toNativeSeparators (p); };

    QFileInfo const source {item.from};
    if (item.from.isEmpty () || !source.isFile () || (item.non_empty && source.size () <= 0))
      {
        return QStringLiteral ("nothing to copy: ") + (item.from.isEmpty () ? native (item.to) : native (item.from));
      }

    QString note;
    QFileInfo const target {item.to};
    if (target.exists ())
      {
        if (item.non_empty && target.size () <= 0)
          {
            // an empty stray, nothing worth keeping
            QFile::remove (item.to);
          }
        else if (!replace)
          {
            return QStringLiteral ("kept, already there: ") + native (item.to);
          }
        else
          {
            auto const backup = item.to + QStringLiteral (".pre-migration-")
              + QDateTime::currentDateTimeUtc ().toString (QStringLiteral ("yyyyMMdd-HHmmss"));
            if (!QFile::rename (item.to, backup))
              {
                qWarning () << "Migration: could not set aside" << item.to << "- left unchanged";
                return QStringLiteral ("left unchanged, could not set it aside: ") + native (item.to);
              }
            note = QStringLiteral (" (the file that was there is now ") + native (backup) + QLatin1Char (')');
          }
      }

    QDir {}.mkpath (target.absolutePath ());
    if (!QFile::copy (item.from, item.to))
      {
        qWarning () << "Migration: unable to copy" << item.from << "to" << item.to;
        return QStringLiteral ("FAILED to copy ") + native (item.from) + QStringLiteral (" to ") + native (item.to);
      }
    QFile::setPermissions (item.to, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    copied = true;
    qInfo () << "Migration: copied" << item.from << "to" << item.to;
    return QStringLiteral ("copied ") + native (item.from) + QStringLiteral (" to ") + native (item.to) + note;
  }

  // Run the migration for one settings file, unless it already ran. Returns,
  // per item, whether it was copied - callers use this to tidy a freshly
  // copied ini. Returns all false when the migration had already run or its
  // marker could not be written.
  inline QList<bool> run_once (QString const& marker_file, QString const& heading, QString const& again
                               , QList<Item> const& items, bool replace)
  {
    QList<bool> copied;
    for (int i = 0; i < items.size (); ++i) copied << false;
    if (QFile::exists (marker_file)) return copied;

    QFile marker {marker_file};
    QDir {}.mkpath (QFileInfo {marker.fileName ()}.absolutePath ());
    if (!marker.open (QIODevice::WriteOnly | QIODevice::Text))
      {
        qWarning () << "Migration: cannot write" << marker.fileName () << "- nothing migrated";
        return copied;
      }

    QTextStream out {&marker};
    out << heading << ", " << QDateTime::currentDateTimeUtc ().toString (Qt::ISODate) << '\n'
        << again << '\n';
    for (int i = 0; i < items.size (); ++i)
      {
        bool item_copied {false};
        out << copy_item (items[i], item_copied, replace) << '\n';
        copied[i] = item_copied;
      }
    return copied;
  }

  inline QList<bool> run (QString const& settings_file, QList<Item> const& items)
  {
    return run_once (marker_for (settings_file)
                     , QStringLiteral ("Settings migration from the old program names")
                     , QStringLiteral ("Delete this file to run the migration again on the next start.")
                     , items, true);
  }

  // The later additions: once per settings file, copying only where the file
  // is missing (or empty, for a non_empty item).
  inline QList<bool> run_additional_files (QString const& settings_file, QList<Item> const& items)
  {
    return run_once (files_marker_for (settings_file)
                     , QStringLiteral ("Additional files from the old program names, copied only where missing")
                     , QStringLiteral ("Delete this file to copy them again, where missing, on the next start.")
                     , items, false);
  }
}

#endif
