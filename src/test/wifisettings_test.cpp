// Tests for the static nmcli parsers behind the Bite DJ Wi-Fi settings page.
// They need neither NetworkManager nor a WifiSettings instance: the class keeps
// every piece of text handling in process-free static functions for exactly
// this reason.
//
// Fixtures marked "Phase 0, verbatim" are nmcli 1.52.1 output captured on
// bitepi on 2026-09-15. Only one network is visible from there, so everything
// else is marked "synthetic": hand-written in the same terse format to cover a
// case the device could not produce.
#include "preferences/wifisettings.h"

#include <gtest/gtest.h>

#include <QList>
#include <QString>
#include <QStringList>

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

} // namespace
