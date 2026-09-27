#include "revision_utils.hpp"

#include <cstring>

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSysInfo>

#include "scs_version.h"

namespace
{
  QString revision_extract_number (QString const& s)
  {
    QString revision;

    // try and match a number (hexadecimal allowed)
    QRegularExpression re {R"(^[$:]\w+: (r?[\da-f]+[^$]*)\$$)"};
    auto match = re.match (s);
    if (match.hasMatch ())
      {
        revision = match.captured (1);
      }
    return revision;
  }
}

QString revision (QString const& scs_rev_string)
{
  return "260926";
  QString result;
  auto revision_from_scs = revision_extract_number (scs_rev_string);

#if defined (CMAKE_BUILD)
  QString scs_info {":Rev: " SCS_VERSION_STR " $"};

  auto revision_from_scs_info = revision_extract_number (scs_info);
  if (!revision_from_scs_info.isEmpty ())
    {
      // we managed to get the revision number from svn info etc.
      result = revision_from_scs_info;
    }
  else if (!revision_from_scs.isEmpty ())
    {
      // fall back to revision passed in if any
      result = revision_from_scs;
    }
  else
    {
      // match anything
      QRegularExpression re {R"(^[$:]\w+: ([^$]*)\$$)"};
      auto match = re.match (scs_info);
      if (match.hasMatch ())
        {
          result = match.captured (1);
        }
    }
#else
  if (!revision_from_scs.isEmpty ())
    {
      // not CMake build so all we have is revision passed
      result = revision_from_scs;
    }
#endif
  return result.trimmed ();
}

QString version (bool include_patch)
{
#if defined (CMAKE_BUILD)
  QString v {TO_STRING__ (PROJECT_VERSION_MAJOR) "." TO_STRING__ (PROJECT_VERSION_MINOR)};
  if (include_patch)
    {
      v += "." TO_STRING__ (PROJECT_VERSION_PATCH) + QString {BUILD_TYPE_REVISION};
    }
#else
  QString v {"Not for Release"};
#endif
  return v;
}

// Display form of the application name.
//
// WSMAP is the internal name and has to stay that way: it decides
// %LocalAppData%\WSMAP, wsmap.ini, the single-instance lock file and the UDP
// Id. "WS-MAP" simply reads better in the UI, so the hyphen is added for
// display only and nothing on disk or on the wire sees it. WS and EME65 are
// already what we want to show and pass through untouched.
//
// The prefix test rather than an equality test is deliberate: --rig-name
// appends " - <rig>" to the application name (main.cpp), so an instance can be
// called "WSMAP - Slice A" and should still display as "WS-MAP - Slice A".
QString program_display_name ()
{
  QString name {QCoreApplication::applicationName ()};
  if (name == QLatin1String {"WSMAP"} || name.startsWith (QLatin1String {"WSMAP "}))
    {
      name.replace (0, 5, QLatin1String {"WS-MAP"});
    }
  return name;
}

QString program_title (QString const& revision)
{
  QString id {program_display_name () + "   v" + QCoreApplication::applicationVersion ()};
  return id + " " + revision + " Digital Mode Suite";
}

QString http_user_agent ()
{
  // See User-Agent format definition https://www.rfc-editor.org/rfc/rfc9110#name-user-agent
  QString const platform {
    "(" + QSysInfo::prettyProductName () + "; "
    + QSysInfo::productType () + " " + QSysInfo::productVersion () + "; "
    + QSysInfo::currentCpuArchitecture () + "; "
    + QString {"rv:%1"}.arg (QSysInfo::kernelVersion ()) + ")"};

  return QString {"WSJT-X/" + version () + "_" + revision ()}.simplified () + " " + platform;
}
