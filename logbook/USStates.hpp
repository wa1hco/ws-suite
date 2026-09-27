#ifndef US_STATES_HPP_
#define US_STATES_HPP_

#include <QString>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <QFuture>
#include <QMutex>
#include <QMutexLocker>
#include <QVector>
#include <QDataStream>
#include <QDebug>

/**
 * @brief Utility class for US state tracking (WAS - Worked All States)
 *
 * Provides static methods for:
 * - Converting Maidenhead grid squares to US states
 * - Validating US callsigns
 * - Looking up state names and abbreviations
 *
 * This class supports the WAS (Worked All States) award tracking feature.
 * All 50 US states are supported, plus DC and territories in the callsign DB.
 */
class USStates
{
public:

  struct Entry {
    QByteArray key;
    quint8 state;
  };
    
  static int cmpUnsigned(QByteArray const& a, QByteArray const& b);
  static QVector<Entry>& callsignDb(); 
  static void preloadDatabaseAsync();
  static QString binaryPath();
  static QMutex& databaseMutex();
  static bool& callsignDatabaseLoaded();
  static bool& preloadStarted();
  static quint8 encodeState(QString const& s);
  static void ensureBinaryUpToDate();
  static bool generateBinaryFromTSV(QString const& tsvPath,
    QString const& binPath);

  /**
   * @brief Get list of all 48 contiguous US state abbreviations + DC
   * @return QStringList of two-letter state codes (AL, AZ, ...)
   */
  static QStringList const allStates()
  {
    return QStringList{
      "AL", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "ID",
      "IL", "IN", "IA", "KS", "KY", "LA", "ME", "MD", "MA", "MI",
      "MN", "MS", "MO", "MT", "NE", "NV", "NH", "NJ", "NM", "NY",
      "NC", "ND", "OH", "OK", "OR", "PA", "RI", "SC", "SD", "TN",
      "TX", "UT", "VT", "VA", "WA", "WV", "WI", "WY", "DC"
    };
  }
  
  
  /**
   * @brief Get full state name from abbreviation
   * @param abbrev Two-letter state abbreviation (e.g., "GA")
   * @return Full state name (e.g., "Georgia") or empty string if not found
   */
  static QString stateName(QString const& abbrev)
  {
    static QMap<QString, QString> const names{
      {"AL", "Alabama"}, {"AZ", "Arizona"}, {"AR", "Arkansas"},
      {"CA", "California"}, {"CO", "Colorado"}, {"CT", "Connecticut"}, {"DE", "Delaware"},
      {"FL", "Florida"}, {"GA", "Georgia"}, {"ID", "Idaho"},
      {"IL", "Illinois"}, {"IN", "Indiana"}, {"IA", "Iowa"}, {"KS", "Kansas"},
      {"KY", "Kentucky"}, {"LA", "Louisiana"}, {"ME", "Maine"}, {"MD", "Maryland"},
      {"MA", "Massachusetts"}, {"MI", "Michigan"}, {"MN", "Minnesota"}, {"MS", "Mississippi"},
      {"MO", "Missouri"}, {"MT", "Montana"}, {"NE", "Nebraska"}, {"NV", "Nevada"},
      {"NH", "New Hampshire"}, {"NJ", "New Jersey"}, {"NM", "New Mexico"}, {"NY", "New York"},
      {"NC", "North Carolina"}, {"ND", "North Dakota"}, {"OH", "Ohio"}, {"OK", "Oklahoma"},
      {"OR", "Oregon"}, {"PA", "Pennsylvania"}, {"RI", "Rhode Island"}, {"SC", "South Carolina"},
      {"SD", "South Dakota"}, {"TN", "Tennessee"}, {"TX", "Texas"}, {"UT", "Utah"},
      {"VT", "Vermont"}, {"VA", "Virginia"}, {"WA", "Washington"}, {"WV", "West Virginia"},
      {"WI", "Wisconsin"}, {"WY", "Wyoming"}, {"DC", "District of Columbia"}
    };
    return names.value(abbrev.toUpper(), QString{});
  }

  /**
   * @brief Convert a Maidenhead grid square to US state
   * @param grid 4 or 6 character grid square (e.g., "EM73" or "EM73vb")
   * @return Two-letter state abbreviation or empty string if not in US
   *
   * Only the first 4 characters are used for the mapping.
   * For border grids, returns the primary (most likely) state.
   */
  static QString gridToState(QString const& grid)
  {
    if (grid.length() < 4) return QString{};

    QString grid4 = grid.left(4).toUpper();
    auto const& mapping = gridMapping();
    return mapping.value(grid4, QString{});
  }

  /**
   * @brief Check if a grid square is within the US
   * @param grid Maidenhead grid square
   * @return true if the grid is in the US, false otherwise
   */
  static bool isUSGrid(QString const& grid)
  {
    return !gridToState(grid).isEmpty();
  }

  /**
   * @brief Check if a callsign appears to be a US callsign
   * @param call Amateur radio callsign
   * @return true if the callsign matches US patterns
   *
   * US callsigns start with A, K, N, or W followed by patterns like:
   * - W1AW, K2ABC, N3XYZ, AA1AA, etc.
   */
  static bool isUSCall(QString const& call)
  {
    if (call.isEmpty()) return false;

    QString c = call.toUpper().trimmed();

    // US callsigns start with A, K, N, or W
    QChar first = c.at(0);
    if (first != 'A' && first != 'K' && first != 'N' && first != 'W') {
      return false;
    }

    // Must have at least 3 characters (e.g., K1A)
    if (c.length() < 3) return false;

    // Find the number in the call
    int numPos = -1;
    for (int i = 1; i < c.length(); ++i) {
      if (c.at(i).isDigit()) {
        numPos = i;
        break;
      }
    }

    // US calls must have a number in position 1 or 2
    if (numPos < 1 || numPos > 2) return false;

    // Must have letters after the number
    if (numPos >= c.length() - 1) return false;

    return true;
  }

  /**
   * @brief Path to the TSV file (downloaded or bundled)
   */
  static QString tsvPath()
  {
    QDir dataPath {QStandardPaths::writableLocation(QStandardPaths::DataLocation)};
    QDir dataPath0 {QCoreApplication::applicationDirPath()};
    if (QFile {dataPath.absoluteFilePath("callsign_states.tsv")}.exists()) {
      return dataPath.absoluteFilePath("callsign_states.tsv");
    } else {
      return dataPath0.absoluteFilePath("callsign_states.tsv");
    }
  }

  /**
   * @brief Check if the binary callsign database file exists
   * @return true if the file exists, false otherwise
   */
  static bool binaryFileExists()
  {
    return QFile::exists(binaryPath());
  }

  /**
   * @brief Reload the callsign states database from file
   *
   * Call this after downloading a new callsign_states.tsv file
   * and regenerating the binary to refresh the in-memory database.
   */
  static void reloadCallsignDatabase();

  /**
   * @brief Check if the callsign states database is loaded in memory
   * @return true if loaded, false otherwise
   */
  static bool callsignDatabaseIsLoaded();

  /**
   * @brief Look up state for a callsign using the local database
   * @param call Amateur radio callsign
   * @return Two-letter state abbreviation or empty string if not found
   *
   * Uses the downloaded callsign_states binary database for offline lookups.
   * This is faster than online API lookups and doesn't require network access.
   */
  static QString callsignToState(QString const& call)
  {
    if (call.isEmpty()) return QString{};

    QString norm = call.toUpper().trimmed();
    norm.replace(QChar::Nbsp, QChar(' '));

    QByteArray key;
    key.reserve(norm.size());
    for (QChar c : norm) {
        ushort u = c.unicode();
        if (u < 128) {
            key.append(char(u));
        } else {
            qDebug() << "NON-ASCII in dxCall:" << call << "char:" << QString(c)
                     << "unicode:" << u;
        }
    }

    quint8 stateCode;
    if (lookupState(key, stateCode)) {
        return decodeState(stateCode);
    }

      return QString{};
  }

  // AE5TC - Begin worked states tracking

  /**
   * @brief Check if a state has been worked before (any band)
   * @param state Two-letter state abbreviation
   * @return true if state has been worked, false if new
   */
  static bool stateWorked(QString const& state)
  {
    if (state.isEmpty()) return true;  // empty state is "not new"
    return workedStatesSet().contains(state.toUpper());
  }

  /**
   * @brief Check if a state has been worked on a specific band
   * @param state Two-letter state abbreviation
   * @param band Band string (e.g., "20m", "40m")
   * @return true if state has been worked on this band
   */
  static bool stateWorkedOnBand(QString const& state, QString const& band)
  {
    if (state.isEmpty() || band.isEmpty()) return true;
    QString key = state.toUpper() + ":" + band.toUpper();
    return workedStatesOnBandSet().contains(key);
  }

  /**
   * @brief Mark a state as worked
   * @param state Two-letter state abbreviation
   */
  static void addWorkedState(QString const& state)
  {
    if (!state.isEmpty()) {
      workedStatesSet().insert(state.toUpper());
    }
  }

  /**
   * @brief Mark a state as worked on a specific band
   * @param state Two-letter state abbreviation
   * @param band Band string (e.g., "20m", "40m")
   */
  static void addWorkedStateOnBand(QString const& state, QString const& band)
  {
    if (!state.isEmpty() && !band.isEmpty()) {
      workedStatesSet().insert(state.toUpper());
      QString key = state.toUpper() + ":" + band.toUpper();
      workedStatesOnBandSet().insert(key);
    }
  }

  /**
   * @brief Clear all worked states (used when reloading ADIF)
   */
  static void clearWorkedStates()
  {
    workedStatesSet().clear();
    workedStatesOnBandSet().clear();
  }

  /**
   * @brief Get count of worked states
   * @return Number of unique states worked
   */
  static int workedStatesCount()
  {
    return workedStatesSet().size();
  }

  /**
   * @brief Get list of worked states
   * @return QSet of worked state abbreviations
   */
  static QSet<QString> const& workedStates()
  {
    return workedStatesSet();
  }

  // AE5TC - End worked states tracking

  private:
  
  // Worked states sets
  static QSet<QString>& workedStatesSet()
  {
    static QSet<QString> worked;
    return worked;
  }

  static QSet<QString>& workedStatesOnBandSet()
  {
    static QSet<QString> workedOnBand;
    return workedOnBand;
  }
  /**
   * @brief Binary search for a callsign in the in-memory database
   * @param call Uppercase ASCII callsign as QByteArray
   * @param outState Output state code if found
   * @return true if found, false otherwise
   */
static bool lookupState(QByteArray const& call, quint8& outState)
{
    auto &db = callsignDb();
      
    int left = 0;
    int right = db.size() - 1;

    while (left <= right) {
        int mid = (left + right) / 2;
        int cmp = USStates::cmpUnsigned(call, db[mid].key);

        if (cmp == 0) {
            outState = db[mid].state;
            return true;
        }
        if (cmp < 0) right = mid - 1;
        else         left = mid + 1;
    }

    // Linear fallback for diagnostics
    for (auto const &e : db) {    
                           
      if (USStates::cmpUnsigned(call, e.key) == 0) {
          outState = e.state;
          return true;
      }           
    } 
    return false;
}

  /**
   * @brief Decode state code to two-letter abbreviation
   */
  static QString decodeState(quint8 code)
  {
    static const QStringList states = {
      "AL","AK","AZ","AR","CA","CO","CT","DE","FL","GA",
      "HI","ID","IL","IN","IA","KS","KY","LA","ME","MD",
      "MA","MI","MN","MS","MO","MT","NE","NV","NH","NJ",
      "NM","NY","NC","ND","OH","OK","OR","PA","RI","SC",
      "SD","TN","TX","UT","VT","VA","WA","WV","WI","WY",
      "DC","PR","GU","VI","AS","MP"
    };

    if (code < states.size()) {
      return states[code];
    }
    return QString{};
  }

  /**
   * @brief Get the grid to state mapping
   * @return Map of 4-char grid squares to state abbreviations
   *
   * This is a subset of US grids for common areas.
   * For comprehensive mapping, use grid_to_state.json.
   */
  static QMap<QString, QString> const& gridMapping()
  {
    static QMap<QString, QString> const mapping{
      // Florida
      {"EM60", "FL"}, {"EM70", "FL"}, {"EM71", "FL"}, {"EM80", "FL"}, {"EM81", "FL"},
      {"EL87", "FL"}, {"EL88", "FL"}, {"EL89", "FL"}, {"EL95", "FL"}, {"EL96", "FL"},
      {"EL97", "FL"}, {"EL98", "FL"}, {"EL99", "FL"},

      // Georgia
      {"EM72", "GA"}, {"EM73", "GA"}, {"EM74", "GA"}, {"EM82", "GA"}, {"EM83", "GA"},
      {"EM84", "GA"}, {"EM92", "GA"}, {"EM93", "GA"}, {"EM94", "GA"},

      // Texas
      {"DM71", "TX"}, {"DM72", "TX"}, {"DM80", "TX"}, {"DM81", "TX"}, {"DM90", "TX"},
      {"DM91", "TX"}, {"EL06", "TX"}, {"EL07", "TX"}, {"EL08", "TX"}, {"EL09", "TX"},
      {"EL16", "TX"}, {"EL17", "TX"}, {"EL18", "TX"}, {"EL19", "TX"}, {"EL29", "TX"},
      {"EM00", "TX"}, {"EM01", "TX"}, {"EM10", "TX"}, {"EM11", "TX"}, {"EM12", "TX"},
      {"EM13", "TX"}, {"EM20", "TX"}, {"EM21", "TX"}, {"EM22", "TX"}, {"EM23", "TX"},

      // California
      {"CM87", "CA"}, {"CM88", "CA"}, {"CM94", "CA"}, {"CM95", "CA"}, {"CM96", "CA"},
      {"CM97", "CA"}, {"CM98", "CA"}, {"CN80", "CA"}, {"CN81", "CA"}, {"CN82", "CA"},
      {"CN83", "CA"}, {"CN84", "CA"}, {"CN85", "CA"}, {"CN90", "CA"}, {"CN91", "CA"},
      {"DM03", "CA"}, {"DM04", "CA"}, {"DM05", "CA"}, {"DM06", "CA"}, {"DM07", "CA"},
      {"DM12", "CA"}, {"DM13", "CA"}, {"DM14", "CA"}, {"DM15", "CA"}, {"DM16", "CA"},

      // New York
      {"FN20", "NY"}, {"FN21", "NY"}, {"FN22", "NY"}, {"FN23", "NY"}, {"FN30", "NY"},
      {"FN31", "NY"}, {"FN32", "NY"}, {"FN12", "NY"}, {"FN13", "NY"}, {"FN02", "NY"},

      // Pennsylvania
      {"EN91", "PA"}, {"EN92", "PA"}, {"FN00", "PA"}, {"FN01", "PA"}, {"FN10", "PA"},
      {"FN11", "PA"}, {"FN20", "PA"}, {"FN21", "PA"},

      // Ohio
      {"EM79", "OH"}, {"EM89", "OH"}, {"EN70", "OH"}, {"EN71", "OH"}, {"EN80", "OH"},
      {"EN81", "OH"}, {"EN90", "OH"}, {"EN91", "OH"},

      // Illinois
      {"EN40", "IL"}, {"EN41", "IL"}, {"EN50", "IL"}, {"EN51", "IL"}, {"EN52", "IL"},
      {"EN60", "IL"}, {"EN61", "IL"}, {"EM49", "IL"}, {"EM58", "IL"}, {"EM59", "IL"},

      // Michigan
      {"EN72", "MI"}, {"EN73", "MI"}, {"EN74", "MI"}, {"EN75", "MI"}, {"EN76", "MI"},
      {"EN82", "MI"}, {"EN83", "MI"}, {"EN84", "MI"}, {"EN85", "MI"}, {"EN86", "MI"},

      // Washington
      {"CN76", "WA"}, {"CN77", "WA"}, {"CN78", "WA"}, {"CN84", "WA"}, {"CN85", "WA"},
      {"CN86", "WA"}, {"CN87", "WA"}, {"CN88", "WA"}, {"CN96", "WA"}, {"CN97", "WA"},
      {"DN06", "WA"}, {"DN07", "WA"}, {"DN08", "WA"}, {"DN16", "WA"}, {"DN17", "WA"},

      // Oregon
      {"CN73", "OR"}, {"CN74", "OR"}, {"CN75", "OR"}, {"CN82", "OR"}, {"CN83", "OR"},
      {"CN84", "OR"}, {"DN00", "OR"}, {"DN01", "OR"}, {"DN02", "OR"}, {"DN10", "OR"},
      {"DN11", "OR"}, {"DN12", "OR"}, {"DN20", "OR"}, {"DN21", "OR"},

      // Colorado
      {"DM57", "CO"}, {"DM58", "CO"}, {"DM59", "CO"}, {"DM67", "CO"}, {"DM68", "CO"},
      {"DM69", "CO"}, {"DM77", "CO"}, {"DM78", "CO"}, {"DM79", "CO"}, {"DN50", "CO"},
      {"DN60", "CO"}, {"DN70", "CO"},

      // Virginia
      {"EM86", "VA"}, {"EM87", "VA"}, {"EM96", "VA"}, {"EM97", "VA"}, {"FM06", "VA"},
      {"FM07", "VA"}, {"FM08", "VA"}, {"FM16", "VA"}, {"FM17", "VA"}, {"FM18", "VA"},

      // North Carolina
      {"EM85", "NC"}, {"EM95", "NC"}, {"FM03", "NC"}, {"FM04", "NC"}, {"FM05", "NC"},
      {"FM06", "NC"}, {"FM14", "NC"}, {"FM15", "NC"},

      // Arizona
      {"DM33", "AZ"}, {"DM34", "AZ"}, {"DM41", "AZ"}, {"DM42", "AZ"}, {"DM43", "AZ"},
      {"DM44", "AZ"}, {"DM51", "AZ"}, {"DM52", "AZ"}, {"DM53", "AZ"}, {"DM54", "AZ"},

      // Minnesota
      {"EN12", "MN"}, {"EN13", "MN"}, {"EN14", "MN"}, {"EN22", "MN"}, {"EN23", "MN"},
      {"EN24", "MN"}, {"EN32", "MN"}, {"EN33", "MN"}, {"EN34", "MN"}, {"EN35", "MN"},

      // Hawaii
      {"BK29", "HI"}, {"BL01", "HI"}, {"BL10", "HI"}, {"BL11", "HI"},

      // Alaska (partial)
      {"BP51", "AK"}, {"BP61", "AK"}, {"BP62", "AK"}, {"BP71", "AK"}, {"BP72", "AK"},

      // Add more as needed...
    };
    return mapping;
  }

  /**
   * @brief Get the grid field (first 2 chars) to possible states mapping
   * @return Map of 2-char field prefixes to lists of possible states
   */
  static QMap<QString, QStringList> const& fieldToStates()
  {
    static QMap<QString, QStringList> const mapping{
      {"CM", {"CA"}},
      {"CN", {"CA", "OR", "WA"}},
      {"DM", {"CA", "NV", "AZ", "NM", "UT", "CO", "TX"}},
      {"DN", {"OR", "WA", "ID", "MT", "WY", "NV", "UT", "CO", "NE", "SD", "ND"}},
      {"EL", {"TX", "FL"}},
      {"EM", {"TX", "OK", "KS", "MO", "AR", "LA", "MS", "AL", "TN", "KY", "IL", "IN", "OH", "WV", "VA", "NC", "SC", "GA", "FL"}},
      {"EN", {"NE", "SD", "ND", "MN", "IA", "WI", "IL", "IN", "MI", "OH", "PA", "NY"}},
      {"FM", {"VA", "NC", "MD", "DE", "NJ", "PA"}},
      {"FN", {"PA", "NY", "NJ", "CT", "RI", "MA", "VT", "NH", "ME"}},
      {"BP", {"AK"}},
      {"BL", {"HI", "AK"}},
    };
    return mapping;
  }
};

#endif // US_STATES_HPP_
