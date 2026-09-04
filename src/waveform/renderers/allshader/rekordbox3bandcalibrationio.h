#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "waveform/renderers/allshader/rekordbox3bandcalibration.h"

namespace mixxx {

/// The calibration every 3Band drawing path uses.
///
/// `Rekordbox3BandCalibration`'s own defaults are what ships and remain the
/// authority. This adds one thing: if a JSON file is sitting in the settings
/// directory it is overlaid on top of them, so the balance can be re-tuned on
/// the appliance with a text editor and a restart instead of a cross compile, a
/// 524MB binary push and a relink. Tuning this renderer is a matter of taste
/// judged on a panel, which means many small iterations, and a three minute
/// build between each of them is the actual obstacle to getting it right.
///
/// Read once, on first use. There is no file watching: a restart is cheap now
/// that a rebuild is not.
///
/// **Both drawing paths must call this.** The scrolling renderer and
/// `WOverview`'s own 3Band path each used to hold a default-constructed
/// calibration of their own, and if only one of them is converted the waveform
/// and the strip underneath it silently disagree.
const Rekordbox3BandCalibration& rekordbox3BandCalibration();

/// The file the above reads, `rekordbox3band-calibration.json` in the settings
/// directory. Exposed so a warning can name it and so tests can assert the name
/// without duplicating the string.
QString rekordbox3BandCalibrationFileName();

/// Overlay a JSON object onto `base`, returning the result.
///
/// Every key is optional: what is absent keeps the value it has in `base`, so a
/// file that changes three numbers is three lines long rather than thirty. That
/// is deliberately more forgiving than the Python rasterizer, which demands a
/// complete file and rejects unknown keys, because that file is a mirror of the
/// C++ defaults checked by a test while this one is something a person edits by
/// hand on a device.
///
/// Nothing here can fail hard. An unparseable value, an unknown key or an
/// out-of-range number appends a line to `pWarnings` and leaves that field
/// alone. A hand-edited file must never be able to blank the waveform, which is
/// the one outcome that would be both confusing and hard to attribute.
Rekordbox3BandCalibration rekordbox3BandCalibrationFromJson(
        const QJsonObject& json,
        const Rekordbox3BandCalibration& base,
        QStringList* pWarnings);

/// Parse a `columnRule` string. Returns false and leaves `*pRule` alone when the
/// name is not one of the four.
bool rekordbox3BandColumnRuleFromString(
        const QString& name, Rekordbox3BandColumnRule* pRule);

} // namespace mixxx
