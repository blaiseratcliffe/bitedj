#include "library/rekordbox/rekordboxtrackhealth.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QStringList>
#include <utility>

namespace mixxx {
namespace rekordbox {

namespace {

// Every ANLZ file, .DAT, .EXT and .2EX alike, opens with this tag.
const QByteArray kAnlzMagic = QByteArrayLiteral("PMAI");

// True when `path` exists but is not a readable ANLZ file. A file that does
// not exist is not damaged: older exports simply have fewer siblings.
bool isDamagedAnlzFile(const QString& path) {
    QFile file(path);
    if (!file.exists()) {
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return true;
    }
    // The file that aborted the app on 2026-09-27 was an .EXT zeroed after its
    // first byte by an unclean unmount; a short read fails this the same way.
    return file.read(kAnlzMagic.size()) != kAnlzMagic;
}

} // anonymous namespace

TrackHealth checkTrackFiles(const QString& location, const QString& anlzDatPath) {
    TrackHealth health;

    // First, because a track with no audio cannot be loaded at all, which
    // matters more than anything wrong with its analysis.
    const QFileInfo audioInfo(location);
    if (!audioInfo.exists() || audioInfo.size() == 0) {
        health.problem = TrackProblem::AudioMissing;
        health.badPath = location;
        return health;
    }

    if (anlzDatPath.isEmpty()) {
        return health;
    }

    QStringList anlzPaths = {anlzDatPath};
    // The same sibling naming getTrack() uses to find the .EXT and .2EX.
    if (anlzDatPath.endsWith(QLatin1String("DAT"), Qt::CaseInsensitive)) {
        const QString stem = anlzDatPath.left(anlzDatPath.length() - 3);
        anlzPaths.append(stem + QLatin1String("EXT"));
        anlzPaths.append(stem + QLatin1String("2EX"));
    }
    for (const QString& anlzPath : std::as_const(anlzPaths)) {
        if (isDamagedAnlzFile(anlzPath)) {
            health.problem = TrackProblem::AnalysisDamaged;
            health.badPath = anlzPath;
            return health;
        }
    }

    return health;
}

} // namespace rekordbox
} // namespace mixxx
