#ifndef REVISION_UTILS_HPP__
#define REVISION_UTILS_HPP__

#include <QString>

QString revision (QString const& svn_rev_string = QString {});
QString version (bool include_patch = true);
// Application name as it should be shown to the user: "WSMAP" displays as
// "WS-MAP". Display only - the internal name still decides the data
// directory, the ini file, the lock file and the UDP Id.
QString program_display_name ();
QString program_title (QString const& revision = QString {});
QString http_user_agent ();

#endif
