#include "waveform/renderers/allshader/rekordbox3bandcalibrationio.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

#include "util/cmdlineargs.h"

namespace {

const QString kFileName = QStringLiteral("rekordbox3band-calibration.json");

/// Read one float key, clamped into a stated range.
///
/// Out of range is a warning and no change rather than a clamp, because a
/// silently clamped number would read back from the file as something the
/// renderer is not using, and the whole point of the file is to be able to
/// trust what it says.
void readFloat(const QJsonObject& json,
        const QString& key,
        float* pField,
        double minimum,
        double maximum,
        QStringList* pWarnings) {
    if (!json.contains(key)) {
        return;
    }
    const QJsonValue value = json.value(key);
    if (!value.isDouble()) {
        pWarnings->append(QStringLiteral("%1 is not a number, ignoring it").arg(key));
        return;
    }
    const double number = value.toDouble();
    if (number < minimum || number > maximum) {
        pWarnings->append(QStringLiteral("%1 is %2, outside %3..%4, ignoring it")
                                  .arg(key)
                                  .arg(number)
                                  .arg(minimum)
                                  .arg(maximum));
        return;
    }
    *pField = static_cast<float>(number);
}

void readColour(const QJsonObject& json,
        const QString& key,
        QColor* pField,
        QStringList* pWarnings) {
    if (!json.contains(key)) {
        return;
    }
    const QJsonValue value = json.value(key);
    if (!value.isString()) {
        pWarnings->append(QStringLiteral("%1 is not a colour string, ignoring it").arg(key));
        return;
    }
    const QColor colour = QColor::fromString(value.toString());
    if (!colour.isValid()) {
        pWarnings->append(QStringLiteral("%1 is %2, which is not a colour, ignoring it")
                                  .arg(key, value.toString()));
        return;
    }
    *pField = colour;
}

} // namespace

namespace mixxx {

QString rekordbox3BandCalibrationFileName() {
    return kFileName;
}

bool rekordbox3BandColumnRuleFromString(
        const QString& name, Rekordbox3BandColumnRule* pRule) {
    if (name == QLatin1String("Nearest")) {
        *pRule = Rekordbox3BandColumnRule::Nearest;
    } else if (name == QLatin1String("MaxOverRange")) {
        *pRule = Rekordbox3BandColumnRule::MaxOverRange;
    } else if (name == QLatin1String("MeanOverRange")) {
        *pRule = Rekordbox3BandColumnRule::MeanOverRange;
    } else if (name == QLatin1String("PunchBlend")) {
        *pRule = Rekordbox3BandColumnRule::PunchBlend;
    } else {
        return false;
    }
    return true;
}

Rekordbox3BandCalibration rekordbox3BandCalibrationFromJson(
        const QJsonObject& json,
        const Rekordbox3BandCalibration& base,
        QStringList* pWarnings) {
    Rekordbox3BandCalibration cal = base;
    QStringList ignored;
    QStringList* pOut = pWarnings ? pWarnings : &ignored;

    readColour(json, QStringLiteral("low"), &cal.low, pOut);
    readColour(json, QStringLiteral("mid"), &cal.mid, pOut);
    readColour(json, QStringLiteral("high"), &cal.high, pOut);
    readColour(json, QStringLiteral("background"), &cal.background, pOut);
    readColour(json, QStringLiteral("lowMid"), &cal.lowMid, pOut);
    readColour(json, QStringLiteral("midHigh"), &cal.midHigh, pOut);
    readColour(json, QStringLiteral("lowHigh"), &cal.lowHigh, pOut);
    readColour(json, QStringLiteral("lowMidHigh"), &cal.lowMidHigh, pOut);

    // The painter-model numbers are documentation, not inputs; nothing computes
    // with them. They are accepted so that a copy of the mirrored JSON loads
    // without complaining about three unknown keys.
    readFloat(json, QStringLiteral("highAlpha"), &cal.highAlpha, 0.0, 1.0, pOut);
    readFloat(json, QStringLiteral("midAlpha"), &cal.midAlpha, 0.0, 1.0, pOut);
    readFloat(json, QStringLiteral("lowBleed"), &cal.lowBleed, 0.0, 1.0, pOut);

    readFloat(json, QStringLiteral("gamma"), &cal.gamma, 0.01, 10.0, pOut);
    readFloat(json, QStringLiteral("lowHeightScale"), &cal.lowHeightScale, 0.0, 8.0, pOut);
    readFloat(json, QStringLiteral("midHeightScale"), &cal.midHeightScale, 0.0, 8.0, pOut);
    readFloat(json, QStringLiteral("highHeightScale"), &cal.highHeightScale, 0.0, 8.0, pOut);
    readFloat(json, QStringLiteral("lowColumnPunch"), &cal.lowColumnPunch, 0.0, 1.0, pOut);
    readFloat(json, QStringLiteral("midColumnPunch"), &cal.midColumnPunch, 0.0, 1.0, pOut);
    readFloat(json, QStringLiteral("highColumnPunch"), &cal.highColumnPunch, 0.0, 1.0, pOut);
    readFloat(json, QStringLiteral("pwv7FullScale"), &cal.pwv7FullScale, 1.0, 255.0, pOut);
    readFloat(json, QStringLiteral("pwv6FullScale"), &cal.pwv6FullScale, 0.0, 255.0, pOut);
    readFloat(json,
            QStringLiteral("minVisibleHeightPx"),
            &cal.minVisibleHeightPx,
            0.0,
            64.0,
            pOut);
    readFloat(json,
            QStringLiteral("antialiasWidthPx"),
            &cal.antialiasWidthPx,
            0.0,
            16.0,
            pOut);
    readFloat(json, QStringLiteral("opacity"), &cal.opacity, 0.0, 1.0, pOut);

    if (json.contains(QStringLiteral("columnRule"))) {
        const QJsonValue value = json.value(QStringLiteral("columnRule"));
        Rekordbox3BandColumnRule rule = cal.columnRule;
        if (!value.isString() ||
                !rekordbox3BandColumnRuleFromString(value.toString(), &rule)) {
            pOut->append(QStringLiteral(
                    "columnRule must be Nearest, MaxOverRange, MeanOverRange or "
                    "PunchBlend, ignoring it"));
        } else {
            cal.columnRule = rule;
        }
    }

    // Anything left over is a typo. Naming it is the difference between "my
    // edit did nothing" and "I spelled it lowHeightscale".
    static const QStringList known{
            QStringLiteral("low"),
            QStringLiteral("mid"),
            QStringLiteral("high"),
            QStringLiteral("background"),
            QStringLiteral("lowMid"),
            QStringLiteral("midHigh"),
            QStringLiteral("lowHigh"),
            QStringLiteral("lowMidHigh"),
            QStringLiteral("highAlpha"),
            QStringLiteral("midAlpha"),
            QStringLiteral("lowBleed"),
            QStringLiteral("gamma"),
            QStringLiteral("lowHeightScale"),
            QStringLiteral("midHeightScale"),
            QStringLiteral("highHeightScale"),
            QStringLiteral("lowColumnPunch"),
            QStringLiteral("midColumnPunch"),
            QStringLiteral("highColumnPunch"),
            QStringLiteral("pwv7FullScale"),
            QStringLiteral("pwv6FullScale"),
            QStringLiteral("minVisibleHeightPx"),
            QStringLiteral("antialiasWidthPx"),
            QStringLiteral("opacity"),
            QStringLiteral("columnRule"),
    };
    const QStringList keys = json.keys();
    for (const QString& key : keys) {
        if (key.startsWith(QLatin1Char('_'))) {
            // Reserved for comments, the same as in the mirrored JSON.
            continue;
        }
        if (!known.contains(key)) {
            pOut->append(QStringLiteral("unknown key %1, ignoring it").arg(key));
        }
    }

    return cal;
}

const Rekordbox3BandCalibration& rekordbox3BandCalibration() {
    // Function-local static: read once, on whichever drawing path asks first.
    static const Rekordbox3BandCalibration calibration = [] {
        const Rekordbox3BandCalibration defaults;
        const QString path =
                QDir(CmdlineArgs::Instance().getSettingsPath()).filePath(kFileName);

        if (!QFile::exists(path)) {
            // Every line here is qWarning() and not qInfo() on purpose.
            // kLogLevelDefault is LogLevel::Warning (util/logging.h) and the
            // appliance launches without --logLevel, so nothing below warning
            // reaches ~/bitedj.log at all. A line that never appears cannot
            // answer "is it reading my file?", which is the only question this
            // feature raises.
            qWarning() << "3Band calibration: built-in defaults, no" << path;
            return defaults;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "3Band calibration: cannot read" << path
                       << file.errorString() << "- using built-in defaults";
            return defaults;
        }
        const QByteArray raw = file.readAll();
        file.close();

        QJsonParseError error{};
        const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            qWarning() << "3Band calibration: cannot parse" << path << ":"
                       << error.errorString() << "- using built-in defaults";
            return defaults;
        }

        QStringList warnings;
        const Rekordbox3BandCalibration loaded = rekordbox3BandCalibrationFromJson(
                document.object(), defaults, &warnings);
        for (const QString& warning : warnings) {
            qWarning() << "3Band calibration:" << path << ":" << warning;
        }
        qWarning() << "3Band calibration: loaded" << path;
        return loaded;
    }();
    return calibration;
}

} // namespace mixxx
