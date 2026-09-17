// Tests for the Bite DJ Wi-Fi settings page's backend, in two halves.
//
// WifiSettingsTest covers the static nmcli parsers and the wrong-password
// classifier. They need neither NetworkManager nor a WifiSettings instance:
// the class keeps every piece of text handling in process-free static
// functions for exactly this reason.
//
// WifiSettingsStateTest (at the bottom, outside the anonymous namespace so the
// friend declaration in wifisettings.h finds it) drives a real instance
// through the paths that change no state outside the object: the tap dispatch,
// the page transitions, the password buffer's bounds and the clamping of
// hostile writes to the [Wifi] COs. Its two safety rules are in the comment on
// the fixture, and both are load-bearing.
//
// Fixtures marked "Phase 0, verbatim" are nmcli 1.52.1 output captured on
// bitepi on 2026-09-15. Only one network is visible from there, so everything
// else is marked "synthetic": hand-written in the same terse format to cover a
// case the device could not produce.
#include "preferences/wifisettings.h"

#include <gtest/gtest.h>

#include <QChar>
#include <QList>
#include <QMetaType>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <limits>
#include <memory>

#include "control/controlobject.h"
#include "test/mixxxtest.h"
#include "util/fpclassify.h"

namespace {

// Phase 0, verbatim: `nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status`.
const QString kDeviceStatus = QStringLiteral(
        "wlan0:wifi:connected:iwanttoridemybicycle\n"
        "lo:loopback:connected (externally):lo\n"
        "p2p-dev-wlan0:wifi-p2p:disconnected:\n"
        "eth0:ethernet:unavailable:\n");

// Phase 0, verbatim: `nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY device wifi list
// --rescan auto`.
const QString kWifiList = QStringLiteral("*:iwanttoridemybicycle:73:WPA2\n");

// Phase 0, verbatim: the same list with BSSID appended (--rescan no), which is
// where terse mode's "\:" escaping shows up.
const QString kWifiListWithBssid =
        QStringLiteral("*:iwanttoridemybicycle:73:WPA2:B4\\:20\\:46\\:80\\:5B\\:06\n");

// Phase 0, verbatim: `nmcli -t -f NAME,TYPE,DEVICE connection show`.
const QString kConnectionShow = QStringLiteral(
        "iwanttoridemybicycle:802-11-wireless:wlan0\n"
        "lo:loopback:lo\n"
        "Wired connection 1:802-3-ethernet:\n");

// Phase 0, verbatim: `nmcli -t -f IP4.ADDRESS device show wlan0`.
const QString kIpv4Wlan0 = QStringLiteral("IP4.ADDRESS[1]:192.168.4.39/22\n");

const QStringList kSavedHome{QStringLiteral("iwanttoridemybicycle")};

// --- splitTerseFields -------------------------------------------------------

TEST(WifiSettingsTest, SplitTerseFieldsSplitsOnColons) {
    const QStringList fields = WifiSettings::splitTerseFields(QStringLiteral("a:b:c"));
    ASSERT_EQ(3, fields.size());
    EXPECT_EQ(QStringLiteral("a"), fields.at(0));
    EXPECT_EQ(QStringLiteral("b"), fields.at(1));
    EXPECT_EQ(QStringLiteral("c"), fields.at(2));
}

TEST(WifiSettingsTest, SplitTerseFieldsKeepsEmptyFields) {
    // Phase 0, verbatim: the p2p row of `device status`, whose CONNECTION is
    // empty and so ends the line with a bare colon.
    const QStringList fields =
            WifiSettings::splitTerseFields(QStringLiteral("p2p-dev-wlan0:wifi-p2p:disconnected:"));
    ASSERT_EQ(4, fields.size());
    EXPECT_EQ(QStringLiteral("disconnected"), fields.at(2));
    EXPECT_TRUE(fields.at(3).isEmpty());

    // Synthetic: an empty line is one empty field, not none.
    const QStringList empty = WifiSettings::splitTerseFields(QString());
    ASSERT_EQ(1, empty.size());
    EXPECT_TRUE(empty.at(0).isEmpty());
}

TEST(WifiSettingsTest, SplitTerseFieldsUnescapesColonsInBssid) {
    // Phase 0, verbatim (the BSSID row).
    const QStringList fields = WifiSettings::splitTerseFields(kWifiListWithBssid.trimmed());
    ASSERT_EQ(5, fields.size());
    EXPECT_EQ(QStringLiteral("*"), fields.at(0));
    EXPECT_EQ(QStringLiteral("iwanttoridemybicycle"), fields.at(1));
    EXPECT_EQ(QStringLiteral("73"), fields.at(2));
    EXPECT_EQ(QStringLiteral("WPA2"), fields.at(3));
    EXPECT_EQ(QStringLiteral("B4:20:46:80:5B:06"), fields.at(4));
}

TEST(WifiSettingsTest, SplitTerseFieldsUnescapesBackslashes) {
    // Synthetic: an SSID holding both escapes, a\:b\\c on the wire.
    const QStringList fields =
            WifiSettings::splitTerseFields(QStringLiteral(" :a\\:b\\\\c:40:WPA2"));
    ASSERT_EQ(4, fields.size());
    EXPECT_EQ(QStringLiteral("a:b\\c"), fields.at(1));
}

TEST(WifiSettingsTest, SplitTerseFieldsKeepsATrailingLoneBackslash) {
    // Synthetic: a backslash with nothing after it escapes nothing.
    const QStringList fields = WifiSettings::splitTerseFields(QStringLiteral("a:b\\"));
    ASSERT_EQ(2, fields.size());
    EXPECT_EQ(QStringLiteral("b\\"), fields.at(1));
}

// --- parseWifiList ----------------------------------------------------------

TEST(WifiSettingsTest, ParseWifiListReadsThePhase0Network) {
    // Phase 0, verbatim.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(kWifiList, kSavedHome);
    ASSERT_EQ(1, rows.size());
    const WifiRow& row = rows.at(0);
    EXPECT_EQ(QStringLiteral("iwanttoridemybicycle"), row.ssid);
    EXPECT_EQ(73, row.signalPercent);
    EXPECT_TRUE(row.secured);
    EXPECT_FALSE(row.enterprise);
    EXPECT_TRUE(row.saved);
    EXPECT_TRUE(row.active);
}

TEST(WifiSettingsTest, ParseWifiListIgnoresFieldsBeyondSecurity) {
    // Phase 0, verbatim: the BSSID variant parses to the same row.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(kWifiListWithBssid, kSavedHome);
    ASSERT_EQ(1, rows.size());
    EXPECT_EQ(QStringLiteral("iwanttoridemybicycle"), rows.at(0).ssid);
    EXPECT_EQ(73, rows.at(0).signalPercent);
    EXPECT_TRUE(rows.at(0).secured);
    EXPECT_TRUE(rows.at(0).active);
}

TEST(WifiSettingsTest, ParseWifiListSavedFlagFollowsTheSavedNames) {
    // Phase 0, verbatim list, with no saved profiles passed in.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(kWifiList, QStringList());
    ASSERT_EQ(1, rows.size());
    EXPECT_FALSE(rows.at(0).saved);
}

TEST(WifiSettingsTest, ParseWifiListDropsHiddenNetworks) {
    // Synthetic: a hidden network has an empty SSID in terse mode.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(
            QStringLiteral(" ::45:WPA2\n"
                           " :Visible:30:WPA2\n"),
            QStringList());
    ASSERT_EQ(1, rows.size());
    EXPECT_EQ(QStringLiteral("Visible"), rows.at(0).ssid);
}

TEST(WifiSettingsTest, ParseWifiListOpenNetworksAreNotSecured) {
    // Synthetic: terse mode leaves SECURITY empty for an open network; the
    // tabular form's "--" is accepted too.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(
            QStringLiteral(" :CafeGuest:60:\n"
                           " :Library:50:--\n"),
            QStringList());
    ASSERT_EQ(2, rows.size());
    for (const WifiRow& row : rows) {
        EXPECT_FALSE(row.secured) << row.ssid.toStdString();
        EXPECT_FALSE(row.enterprise) << row.ssid.toStdString();
        EXPECT_FALSE(row.active) << row.ssid.toStdString();
    }
}

TEST(WifiSettingsTest, ParseWifiListFlagsEnterpriseNetworks) {
    // Synthetic: WPA2 802.1X is secured and enterprise.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(
            QStringLiteral(" :CorpNet:80:WPA2 802.1X\n"), QStringList());
    ASSERT_EQ(1, rows.size());
    EXPECT_TRUE(rows.at(0).secured);
    EXPECT_TRUE(rows.at(0).enterprise);
}

TEST(WifiSettingsTest, ParseWifiListPersonalSecurityVariantsAreSecuredNotEnterprise) {
    // Synthetic: mixed-mode WPA1 WPA2, and WPA3.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(
            QStringLiteral(" :OldRouter:40:WPA1 WPA2\n"
                           " :NewRouter:70:WPA3\n"),
            QStringList());
    ASSERT_EQ(2, rows.size());
    for (const WifiRow& row : rows) {
        EXPECT_TRUE(row.secured) << row.ssid.toStdString();
        EXPECT_FALSE(row.enterprise) << row.ssid.toStdString();
    }
}

TEST(WifiSettingsTest, ParseWifiListUnescapesSsids) {
    // Synthetic: an SSID containing both terse escapes, and a saved profile
    // of the same (unescaped) name.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(
            QStringLiteral(" :a\\:b\\\\c:40:WPA2\n"), QStringList{QStringLiteral("a:b\\c")});
    ASSERT_EQ(1, rows.size());
    EXPECT_EQ(QStringLiteral("a:b\\c"), rows.at(0).ssid);
    EXPECT_TRUE(rows.at(0).saved);
}

TEST(WifiSettingsTest, ParseWifiListKeepsTheStrongestBssid) {
    // Synthetic: one mesh network seen on three access points.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(
            QStringLiteral(" :Mesh:40:WPA2\n"
                           " :Mesh:70:WPA2\n"
                           " :Mesh:55:WPA2\n"),
            QStringList());
    ASSERT_EQ(1, rows.size());
    EXPECT_EQ(QStringLiteral("Mesh"), rows.at(0).ssid);
    EXPECT_EQ(70, rows.at(0).signalPercent);
}

TEST(WifiSettingsTest, ParseWifiListPrefersTheInUseBssidOverAStrongerOne) {
    // Synthetic: the box is associated with the weaker access point. The row
    // must stay active, whichever order nmcli lists them in.
    const QList<WifiRow> activeFirst = WifiSettings::parseWifiList(
            QStringLiteral("*:Mesh:40:WPA2\n"
                           " :Mesh:70:WPA2\n"),
            QStringList());
    ASSERT_EQ(1, activeFirst.size());
    EXPECT_TRUE(activeFirst.at(0).active);
    EXPECT_EQ(40, activeFirst.at(0).signalPercent);

    const QList<WifiRow> activeLast = WifiSettings::parseWifiList(
            QStringLiteral(" :Mesh:70:WPA2\n"
                           "*:Mesh:40:WPA2\n"),
            QStringList());
    ASSERT_EQ(1, activeLast.size());
    EXPECT_TRUE(activeLast.at(0).active);
    EXPECT_EQ(40, activeLast.at(0).signalPercent);
}

TEST(WifiSettingsTest, ParseWifiListOrdersActiveSavedSignalThenName) {
    // Synthetic: every tier of the sort key, listed in scrambled order.
    const QList<WifiRow> rows = WifiSettings::parseWifiList(
            QStringLiteral(" :zulu:90:WPA2\n"
                           " :SavedWeak:20:WPA2\n"
                           " :bravo:60:WPA2\n"
                           "*:Home:30:WPA2\n"
                           " :Alpha:60:WPA2\n"
                           " :SavedStrong:80:WPA2\n"),
            QStringList{QStringLiteral("SavedWeak"), QStringLiteral("SavedStrong")});
    ASSERT_EQ(6, rows.size());
    // Active first, even though it is weak and not in the saved list.
    EXPECT_EQ(QStringLiteral("Home"), rows.at(0).ssid);
    // Then saved, strongest first.
    EXPECT_EQ(QStringLiteral("SavedStrong"), rows.at(1).ssid);
    EXPECT_EQ(QStringLiteral("SavedWeak"), rows.at(2).ssid);
    // Then by signal.
    EXPECT_EQ(QStringLiteral("zulu"), rows.at(3).ssid);
    // Then equal signal by name, ignoring case.
    EXPECT_EQ(QStringLiteral("Alpha"), rows.at(4).ssid);
    EXPECT_EQ(QStringLiteral("bravo"), rows.at(5).ssid);
}

TEST(WifiSettingsTest, ParseWifiListSkipsMalformedAndBlankLines) {
    // Synthetic: blank lines, a truncated row, CRLF endings, and a signal
    // that is not a number (read as 0) or out of range (clamped).
    const QList<WifiRow> rows = WifiSettings::parseWifiList(
            QStringLiteral("\n"
                           " :Truncated:50\r\n"
                           "\r\n"
                           " :NoSignal:abc:WPA2\r\n"
                           " :TooStrong:150:WPA2\r\n"),
            QStringList());
    ASSERT_EQ(2, rows.size());
    EXPECT_EQ(QStringLiteral("TooStrong"), rows.at(0).ssid);
    EXPECT_EQ(100, rows.at(0).signalPercent);
    EXPECT_EQ(QStringLiteral("NoSignal"), rows.at(1).ssid);
    EXPECT_EQ(0, rows.at(1).signalPercent);
    EXPECT_TRUE(rows.at(1).secured);
}

TEST(WifiSettingsTest, ParseWifiListOfNothingIsEmpty) {
    // Synthetic: no networks in range.
    EXPECT_TRUE(WifiSettings::parseWifiList(QString(), QStringList()).isEmpty());
}

// --- parseSavedWifiNames ----------------------------------------------------

TEST(WifiSettingsTest, ParseSavedWifiNamesKeepsOnlyWifiProfiles) {
    // Phase 0, verbatim: loopback and ethernet profiles are not wifi.
    const QStringList names = WifiSettings::parseSavedWifiNames(kConnectionShow);
    ASSERT_EQ(1, names.size());
    EXPECT_EQ(QStringLiteral("iwanttoridemybicycle"), names.at(0));
}

TEST(WifiSettingsTest, ParseSavedWifiNamesUnescapesNames) {
    // Synthetic: a profile name with a colon in it, two-field NAME,TYPE form.
    const QStringList names = WifiSettings::parseSavedWifiNames(
            QStringLiteral("Cafe\\: Upstairs:802-11-wireless\n"
                           "Wired connection 1:802-3-ethernet\n"));
    ASSERT_EQ(1, names.size());
    EXPECT_EQ(QStringLiteral("Cafe: Upstairs"), names.at(0));
}

TEST(WifiSettingsTest, ParseSavedWifiNamesOfNothingIsEmpty) {
    // Synthetic: no profiles at all.
    EXPECT_TRUE(WifiSettings::parseSavedWifiNames(QString()).isEmpty());
}

// --- parseActiveProfile -----------------------------------------------------

TEST(WifiSettingsTest, ParseActiveProfileFindsTheProfileOnTheWifiDevice) {
    // Phase 0, verbatim: NAME,TYPE,DEVICE, which is the join snapshot's form.
    EXPECT_EQ(QStringLiteral("iwanttoridemybicycle"),
            WifiSettings::parseActiveProfile(kConnectionShow, QStringLiteral("wlan0")));
}

TEST(WifiSettingsTest, ParseActiveProfileOfAnIdleDeviceIsEmpty) {
    // Phase 0, verbatim: "Wired connection 1" is saved but active on nothing,
    // so eth0 has no active profile. An empty device name matches nothing.
    EXPECT_TRUE(WifiSettings::parseActiveProfile(kConnectionShow, QStringLiteral("eth0"))
                        .isEmpty());
    EXPECT_TRUE(WifiSettings::parseActiveProfile(kConnectionShow, QString()).isEmpty());
}

TEST(WifiSettingsTest, ParseActiveProfileUnescapesTheName) {
    // Synthetic: a disconnected box whose only wifi profile is idle, then the
    // same profile, with a colon in its name, active on wlan0.
    EXPECT_TRUE(WifiSettings::parseActiveProfile(
            QStringLiteral("Cafe\\: Upstairs:802-11-wireless:\n"), QStringLiteral("wlan0"))
                        .isEmpty());
    EXPECT_EQ(QStringLiteral("Cafe: Upstairs"),
            WifiSettings::parseActiveProfile(
                    QStringLiteral("Cafe\\: Upstairs:802-11-wireless:wlan0\n"),
                    QStringLiteral("wlan0")));
}

// --- parseWifiDevice --------------------------------------------------------

TEST(WifiSettingsTest, ParseWifiDeviceFindsTheWifiInterface) {
    // Phase 0, verbatim.
    const auto device = WifiSettings::parseWifiDevice(kDeviceStatus);
    ASSERT_TRUE(device.has_value());
    EXPECT_EQ(QStringLiteral("wlan0"), device->device);
    EXPECT_EQ(QStringLiteral("connected"), device->state);
    EXPECT_EQ(QStringLiteral("iwanttoridemybicycle"), device->connection);
}

TEST(WifiSettingsTest, ParseWifiDeviceReadsADisconnectedInterface) {
    // Synthetic: the same box after `device disconnect`.
    const auto device = WifiSettings::parseWifiDevice(
            QStringLiteral("wlan0:wifi:disconnected:\n"
                           "lo:loopback:connected (externally):lo\n"));
    ASSERT_TRUE(device.has_value());
    EXPECT_EQ(QStringLiteral("wlan0"), device->device);
    EXPECT_EQ(QStringLiteral("disconnected"), device->state);
    EXPECT_TRUE(device->connection.isEmpty());
}

TEST(WifiSettingsTest, ParseWifiDeviceIgnoresTheP2pPseudoDevice) {
    // Synthetic: a box whose only wifi-ish row is the p2p device, which is
    // not an adapter that can join anything. That is state 2, no adapter.
    EXPECT_FALSE(WifiSettings::parseWifiDevice(
            QStringLiteral("lo:loopback:connected (externally):lo\n"
                           "p2p-dev-wlan0:wifi-p2p:disconnected:\n"
                           "eth0:ethernet:unavailable:\n"))
                         .has_value());
}

TEST(WifiSettingsTest, ParseWifiDeviceOfNothingIsNone) {
    // Synthetic: empty output.
    EXPECT_FALSE(WifiSettings::parseWifiDevice(QString()).has_value());
}

// --- parseIpv4 --------------------------------------------------------------

TEST(WifiSettingsTest, ParseIpv4StripsThePrefixLength) {
    // Phase 0, verbatim.
    EXPECT_EQ(QStringLiteral("192.168.4.39"), WifiSettings::parseIpv4(kIpv4Wlan0));
}

TEST(WifiSettingsTest, ParseIpv4OfAnUnpluggedDeviceIsEmpty) {
    // Phase 0, verbatim: eth0 unplugged prints nothing at all.
    EXPECT_TRUE(WifiSettings::parseIpv4(QString()).isEmpty());
}

TEST(WifiSettingsTest, ParseIpv4IgnoresAnErrorMessage) {
    // Phase 0, verbatim text, but nmcli prints it on stderr, not stdout.
    // Synthetic in that it is fed to the parser, as a guard against a caller
    // ever handing it the wrong stream.
    EXPECT_TRUE(WifiSettings::parseIpv4(QStringLiteral("Error: Device 'nosuch' not found.\n"))
                        .isEmpty());
}

TEST(WifiSettingsTest, ParseIpv4TakesTheFirstOfSeveralAddresses) {
    // Synthetic: two addresses on the interface.
    EXPECT_EQ(QStringLiteral("10.0.0.5"),
            WifiSettings::parseIpv4(QStringLiteral("IP4.ADDRESS[1]:10.0.0.5/24\n"
                                                   "IP4.ADDRESS[2]:10.0.0.6/24\n")));
}

// --- isWrongPasswordError ---------------------------------------------------
// Not measured on bitepi (Ruling 2 skipped the live wrong-password probe).
// The strings are NetworkManager's own wording, as named in the Phase 1 fix
// round; the SSID-bearing ones are synthetic.

TEST(WifiSettingsTest, WrongPasswordMatchesMissingSecrets) {
    EXPECT_TRUE(WifiSettings::isWrongPasswordError(QStringLiteral(
            "Error: Connection activation failed: Secrets were required, but not provided.")));
}

TEST(WifiSettingsTest, WrongPasswordMatchesAnInvalidPskProperty) {
    EXPECT_TRUE(WifiSettings::isWrongPasswordError(
            QStringLiteral("802-11-wireless-security.psk: property is invalid")));
}

TEST(WifiSettingsTest, WrongPasswordMatchesNewerNmcliWording) {
    // Synthetic SSID in newer nmcli's wording of the same failure.
    EXPECT_TRUE(WifiSettings::isWrongPasswordError(QStringLiteral(
            "Passwords or encryption keys are required to access the wireless network 'x'.")));
}

TEST(WifiSettingsTest, WrongPasswordIgnoresOtherFailures) {
    EXPECT_FALSE(WifiSettings::isWrongPasswordError(
            QStringLiteral("Error: No network with SSID 'x' found.")));
    EXPECT_FALSE(WifiSettings::isWrongPasswordError(QString()));
}

TEST(WifiSettingsTest, WrongPasswordIgnoresPskInAnSsid) {
    // Synthetic: a network whose name contains "psk", in a failure that has
    // nothing to do with its password. A bare "psk" match would misfire here.
    EXPECT_FALSE(WifiSettings::isWrongPasswordError(
            QStringLiteral("Error: No network with SSID 'mypsknet' found.")));
    EXPECT_FALSE(WifiSettings::isWrongPasswordError(QStringLiteral(
            "Error: Connection activation failed: (53) The Wi-Fi network 'mypsknet' "
            "could not be found")));
}

} // namespace

// The state-machine half. Two rules keep a real WifiSettings safe in a harness
// that has no NetworkManager, and nothing below may break either:
//
//  - The event loop is never turned. The constructor schedules its one status
//    read with QTimer::singleShot(0, ...), so no processEvents(), no
//    QSignalSpy::wait() and no exec() anywhere here, and that read never fires.
//  - No test walks a path that starts nmcli. That rules out joining a saved or
//    an open network, submitting a password of a legal length, Disconnect,
//    Forget, Turn Wi-Fi on, and every scan. Those are exercised on the Pi over
//    the control socket and in the hand-over session at the panel, not here.
//
// WifiSettings names this fixture a friend, which is what lets the rows a tap
// acts on be seeded without a scan, and the page and selected index be read
// back without a skin.
class WifiSettingsStateTest : public MixxxTest {
  protected:
    std::unique_ptr<WifiSettings> newSettings() {
        return std::make_unique<WifiSettings>(config());
    }

    // The skin's and the control socket's route to the [Wifi] COs: a write
    // from outside, which is where a hostile value would come from.
    static void writeCo(const char* item, double value) {
        ControlObject::set(ConfigKey(QStringLiteral("[Wifi]"), QString::fromLatin1(item)), value);
    }
    static double readCo(const char* item) {
        return ControlObject::get(ConfigKey(QStringLiteral("[Wifi]"), QString::fromLatin1(item)));
    }

    // Two secured networks that nothing has saved and the box is on neither:
    // the one kind of row whose tap runs no nmcli, because it opens the keypad
    // instead of joining.
    static QList<WifiRow> twoNewSecuredNetworks() {
        QList<WifiRow> rows;
        WifiRow alpha;
        alpha.ssid = QStringLiteral("Alpha");
        alpha.signalPercent = 80;
        alpha.secured = true;
        rows.append(alpha);
        WifiRow bravo;
        bravo.ssid = QStringLiteral("Bravo");
        bravo.signalPercent = 40;
        bravo.secured = true;
        rows.append(bravo);
        return rows;
    }

    // The only way into the rows that does not run a scan: publishRows() is
    // what a landed scan calls, with the connection the rows were seen on --
    // empty here, matching the disconnected state a fresh instance starts in.
    static void seedRows(WifiSettings* pSettings, const QList<WifiRow>& rows) {
        pSettings->publishRows(rows, QString());
    }
    static int pageOf(const WifiSettings* pSettings) {
        return pSettings->page();
    }
    static int selectedIndexOf(const WifiSettings* pSettings) {
        return pSettings->selectedIndex();
    }

    // What the keypad does, one key at a time.
    static void type(WifiSettings* pSettings, const QString& text) {
        for (const QChar c : text) {
            pSettings->appendPasswordChar(c);
        }
    }
};

TEST_F(WifiSettingsStateTest, AFreshInstanceIsOnTheListAndHasReadNothing) {
    const auto pSettings = newSettings();

    EXPECT_EQ(WifiSettings::kStateDisconnected, pSettings->state());
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_TRUE(pSettings->rows().isEmpty());
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
    EXPECT_EQ(0, pSettings->passwordLength());
    EXPECT_QSTRING_EQ(QStringLiteral("Not connected"), pSettings->statusLine());
    EXPECT_EQ(0.0, readCo("network_count"));
    // No row to select yet, whatever the CO holds.
    EXPECT_EQ(-1, selectedIndexOf(pSettings.get()));
}

TEST_F(WifiSettingsStateTest, TappingANewSecuredNetworkOpensTheKeypadForIt) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    QSignalSpy targetSpy(pSettings.get(), &WifiSettings::joinTargetChanged);

    pSettings->activateRow(1);

    EXPECT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    EXPECT_QSTRING_EQ(QStringLiteral("Bravo"), pSettings->joinTarget());
    EXPECT_EQ(0, pSettings->passwordLength());
    // A tap straight from the list widget still moves the CO, so a later
    // [Wifi],join acts on the row the DJ touched.
    EXPECT_EQ(1, selectedIndexOf(pSettings.get()));
    ASSERT_EQ(1, targetSpy.count());
    EXPECT_QSTRING_EQ(QStringLiteral("Bravo"), targetSpy.at(0).at(0).toString());
}

TEST_F(WifiSettingsStateTest, TappingARowThatDoesNotExistDoesNothing) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    QSignalSpy targetSpy(pSettings.get(), &WifiSettings::joinTargetChanged);

    pSettings->activateRow(-1);
    pSettings->activateRow(2);
    pSettings->activateRow(std::numeric_limits<int>::max());
    pSettings->activateRow(std::numeric_limits<int>::min());

    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
    EXPECT_EQ(0, targetSpy.count());
}

TEST_F(WifiSettingsStateTest, TappingAnEnterpriseNetworkStaysOnTheList) {
    const auto pSettings = newSettings();
    QList<WifiRow> rows;
    WifiRow corporate;
    corporate.ssid = QStringLiteral("CorpNet");
    corporate.signalPercent = 70;
    corporate.secured = true;
    corporate.enterprise = true;
    rows.append(corporate);
    seedRows(pSettings.get(), rows);

    pSettings->activateRow(0);

    // Notified and left where they were: the keypad cannot enter a username,
    // a certificate or an inner authentication method.
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
}

TEST_F(WifiSettingsStateTest, ARowScannedActiveIsNotActiveOnADisconnectedBox) {
    const auto pSettings = newSettings();
    QList<WifiRow> rows = twoNewSecuredNetworks();
    // What the scan saw before the link dropped. The box is in state 0 now,
    // so this flag describes a connection it no longer has -- which is why
    // the list widget styles the row from isActiveNow() rather than from it.
    rows[0].active = true;
    seedRows(pSettings.get(), rows);

    EXPECT_FALSE(pSettings->isActiveNow(rows.at(0)));
    // And the tap agrees with the highlight: the keypad, not the manage page.
    pSettings->activateRow(0);
    EXPECT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
}

TEST_F(WifiSettingsStateTest, TheJoinControlOnTheListActivatesTheSelectedRow) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());

    writeCo("selected_index", 1.0);
    writeCo("join", 1.0);

    EXPECT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    EXPECT_QSTRING_EQ(QStringLiteral("Bravo"), pSettings->joinTarget());
    // Put back, or the next write of the same 1 could be dropped as a no-op.
    EXPECT_EQ(0.0, readCo("join"));
}

TEST_F(WifiSettingsStateTest, PasswordKeysAreIgnoredOffTheEntryPage) {
    const auto pSettings = newSettings();
    QSignalSpy passwordSpy(pSettings.get(), &WifiSettings::passwordChanged);

    // Page 0, the list.
    type(pSettings.get(), QStringLiteral("abcdefg"));
    pSettings->backspacePassword();
    EXPECT_EQ(0, pSettings->passwordLength());
    EXPECT_EQ(0, passwordSpy.count());

    // Page 3, the manage page, written straight to the CO the way the control
    // socket would.
    writeCo("page", static_cast<double>(WifiSettings::kPageManage));
    type(pSettings.get(), QStringLiteral("abcdefg"));
    EXPECT_EQ(0, pSettings->passwordLength());
    EXPECT_EQ(0, passwordSpy.count());
}

TEST_F(WifiSettingsStateTest, ThePasswordBufferStopsAtSixtyThreeCharacters) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(0);
    // After the tap, so the clearPassword() it emits is not counted.
    QSignalSpy passwordSpy(pSettings.get(), &WifiSettings::passwordChanged);

    type(pSettings.get(), QString(70, QLatin1Char('a')));

    // 63 is the WPA-PSK maximum; the 64th key onwards changes nothing at all,
    // not even a signal the keypad would repaint on.
    EXPECT_EQ(63, pSettings->passwordLength());
    EXPECT_EQ(63, passwordSpy.count());
    pSettings->appendPasswordChar(QLatin1Char('b'));
    EXPECT_EQ(63, pSettings->passwordLength());
    EXPECT_EQ(63, passwordSpy.count());
}

TEST_F(WifiSettingsStateTest, OnlyPrintableAsciiReachesThePasswordBuffer) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(0);
    QSignalSpy passwordSpy(pSettings.get(), &WifiSettings::passwordChanged);

    // A NUL would truncate the argument nmcli receives; the controls and the
    // non-ASCII characters are not passphrase material either.
    const QList<QChar> rejected{QChar(), // a default QChar is NUL
            QChar(0x0009),
            QChar(0x000A),
            QChar(0x001F),
            QChar(0x007F),
            QChar(0x00E9),
            QChar(0x20AC)};
    for (const QChar c : rejected) {
        pSettings->appendPasswordChar(c);
    }
    EXPECT_EQ(0, pSettings->passwordLength());
    EXPECT_EQ(0, passwordSpy.count());

    // Both ends of the accepted range, space included.
    pSettings->appendPasswordChar(QChar(0x0020));
    pSettings->appendPasswordChar(QChar(0x007E));
    EXPECT_EQ(2, pSettings->passwordLength());
    EXPECT_EQ(2, passwordSpy.count());
}

TEST_F(WifiSettingsStateTest, BackspaceOnAnEmptyBufferDoesNothing) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(0);
    QSignalSpy passwordSpy(pSettings.get(), &WifiSettings::passwordChanged);

    pSettings->backspacePassword();
    EXPECT_EQ(0, pSettings->passwordLength());
    EXPECT_EQ(0, passwordSpy.count());

    type(pSettings.get(), QStringLiteral("ab"));
    pSettings->backspacePassword();
    pSettings->backspacePassword();
    EXPECT_EQ(0, pSettings->passwordLength());
    EXPECT_EQ(4, passwordSpy.count());
    // Empty again: still nothing to undo, and no signal.
    pSettings->backspacePassword();
    EXPECT_EQ(4, passwordSpy.count());
}

TEST_F(WifiSettingsStateTest, PasswordChangedCarriesTheLengthAndNothingElse) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(0);
    QSignalSpy passwordSpy(pSettings.get(), &WifiSettings::passwordChanged);

    const QString typed = QStringLiteral("hunter2");
    type(pSettings.get(), typed);

    ASSERT_EQ(typed.size(), passwordSpy.count());
    for (int i = 0; i < passwordSpy.count(); ++i) {
        // One int argument, which is the whole point: the plaintext never
        // leaves the class, so there is nowhere for a log or a skin to pick
        // it up.
        ASSERT_EQ(1, passwordSpy.at(i).size());
        EXPECT_EQ(static_cast<int>(QMetaType::Int), passwordSpy.at(i).at(0).typeId());
        EXPECT_EQ(i + 1, passwordSpy.at(i).at(0).toInt());
    }
}

TEST_F(WifiSettingsStateTest, AShortPasswordIsRefusedAndKeepsTheKeypadOpen) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(0);
    // Seven characters, deliberately: the eighth would make this submit a
    // legal passphrase and start nmcli, which this harness must never do.
    type(pSettings.get(), QStringLiteral("short12"));

    pSettings->submitJoin();

    // Refused with a notification, and the DJ keeps what they typed rather
    // than starting again.
    EXPECT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    EXPECT_EQ(7, pSettings->passwordLength());
    EXPECT_QSTRING_EQ(QStringLiteral("Alpha"), pSettings->joinTarget());
}

TEST_F(WifiSettingsStateTest, SubmittingWithNoTargetDoesNothing) {
    const auto pSettings = newSettings();
    // Page 1 with no target is only reachable by writing the CO from outside.
    // The guard is what stops a join of the empty SSID from being started.
    writeCo("page", static_cast<double>(WifiSettings::kPagePassword));

    pSettings->submitJoin();

    EXPECT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
}

TEST_F(WifiSettingsStateTest, CancellingTheKeypadClearsTheBufferAndReturnsToTheList) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(0);
    type(pSettings.get(), QStringLiteral("abcd"));

    pSettings->cancelJoin();

    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
    EXPECT_EQ(0, pSettings->passwordLength());
}

TEST_F(WifiSettingsStateTest, HostileSelectedIndexWritesNameNoRow) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());

    // util_double_nan() rather than the numeric_limits constant: this file is
    // compiled with -ffast-math like the rest, and fpclassify.h is where the
    // codebase keeps the NaN handling that survives it.
    const double nan = util_double_nan();
    for (const double value : {-1.0, -0.5, 2.0, 1.0e9, nan}) {
        writeCo("selected_index", value);
        EXPECT_EQ(-1, selectedIndexOf(pSettings.get())) << value;
    }
    // In range, truncated towards the row it names.
    writeCo("selected_index", 1.9);
    EXPECT_EQ(1, selectedIndexOf(pSettings.get()));

    // And a join aimed at no row is a no-op that still puts its CO back.
    writeCo("selected_index", 1.0e9);
    writeCo("join", 1.0);
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
    EXPECT_EQ(0.0, readCo("join"));
}

TEST_F(WifiSettingsStateTest, HostilePageWritesAreNoPageAtAll) {
    const auto pSettings = newSettings();
    const double nan = util_double_nan();
    for (const double value : {-1.0, 4.0, 99.0, 1.0e9, nan}) {
        writeCo("page", value);
        EXPECT_EQ(-1, pageOf(pSettings.get())) << value;
    }

    // Everything that keys on the page declines to act on one that is not a
    // page, rather than falling through to the list's behaviour.
    QSignalSpy passwordSpy(pSettings.get(), &WifiSettings::passwordChanged);
    type(pSettings.get(), QStringLiteral("abc"));
    pSettings->backspacePassword();
    pSettings->submitJoin();
    pSettings->cancelJoin();
    EXPECT_EQ(0, pSettings->passwordLength());
    EXPECT_EQ(0, passwordSpy.count());
    EXPECT_EQ(-1, pageOf(pSettings.get()));
}
