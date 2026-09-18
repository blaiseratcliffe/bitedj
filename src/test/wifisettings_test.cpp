// Tests for the Bite DJ Wi-Fi settings page's backend, in two halves.
//
// WifiSettingsTest covers the static nmcli parsers and the wrong-password
// classifier. They need neither NetworkManager nor a WifiSettings instance:
// the class keeps every piece of text handling in process-free static
// functions for exactly this reason.
//
// WifiSettingsStateTest (at the bottom, outside the anonymous namespace so the
// friend declaration in wifisettings.h finds it) drives a real instance with
// nmcli replaced by a scripted fake: the tap dispatch, the page transitions,
// the password buffer's bounds, the clamping of hostile writes to the [Wifi]
// COs, and the guards on joining, cancelling, disconnecting and forgetting
// that keep the box's only link up. Its safety rules are in the comment on
// the fixture, and they are load-bearing.
//
// Fixtures marked "Phase 0, verbatim" are nmcli 1.52.1 output captured on
// bitepi on 2026-09-15, and "measured" ones the same nmcli on 2026-09-18.
// Only one network is visible from there, so everything else is marked
// "synthetic": hand-written in the same terse format to cover a case the
// device could not produce.
#include "preferences/wifisettings.h"

#include <gtest/gtest-spi.h>
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
#include <utility>

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

// Measured: `nmcli -t -f IP4.ADDRESS,GENERAL.CON-UUID device show wlan0`, the
// status refresh's last read.
const QString kDeviceShowWlan0 = QStringLiteral(
        "IP4.ADDRESS[1]:192.168.4.39/22\n"
        "GENERAL.CON-UUID:a0b1e9f8-84e5-4e83-9837-6ca90bdc30ce\n");

// Measured: `nmcli -t -f NAME,UUID,TYPE connection show`, the scan's first
// step.
const QString kHomeUuid = QStringLiteral("a0b1e9f8-84e5-4e83-9837-6ca90bdc30ce");
const QString kProfileList = QStringLiteral(
        "iwanttoridemybicycle:a0b1e9f8-84e5-4e83-9837-6ca90bdc30ce:802-11-wireless\n"
        "lo:bcb61ff9-40e5-4c9c-af54-68210da7f772:loopback\n"
        "Wired connection 1:1eb7b59f-356f-3501-b5ba-54f6f75444c1:802-3-ethernet\n");

// Measured: `nmcli -t -f connection.id,connection.uuid,802-11-wireless.ssid
// connection show uuid <wifi> uuid <ethernet>`, the scan's second step, asked
// here for an ethernet profile too to see what one without the field prints.
// One block per profile, one empty line between blocks, none after the last,
// and the ethernet block simply has no SSID line.
const QString kProfileDetails = QStringLiteral(
        "connection.id:iwanttoridemybicycle\n"
        "connection.uuid:a0b1e9f8-84e5-4e83-9837-6ca90bdc30ce\n"
        "802-11-wireless.ssid:iwanttoridemybicycle\n"
        "\n"
        "connection.id:Wired connection 1\n"
        "connection.uuid:1eb7b59f-356f-3501-b5ba-54f6f75444c1\n");

// The SSIDs of the saved profiles, which is what parseWifiList() matches a
// row's saved flag against. On bitepi the one profile's name and SSID agree.
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

// --- parseWifiProfileList ---------------------------------------------------
// The first three keep the names they had when this step was
// parseSavedWifiNames(), which read the same listing without the UUID, so the
// test baseline still matches them.

TEST(WifiSettingsTest, ParseSavedWifiNamesKeepsOnlyWifiProfiles) {
    // Measured: loopback and ethernet profiles are not wifi.
    const QList<WifiSettings::SavedProfile> profiles =
            WifiSettings::parseWifiProfileList(kProfileList);
    ASSERT_EQ(1, profiles.size());
    EXPECT_QSTRING_EQ(QStringLiteral("iwanttoridemybicycle"), profiles.at(0).name);
    EXPECT_QSTRING_EQ(kHomeUuid, profiles.at(0).uuid);
    // This listing cannot show an SSID; the second step fills it in.
    EXPECT_TRUE(profiles.at(0).ssid.isEmpty());
}

TEST(WifiSettingsTest, ParseSavedWifiNamesUnescapesNames) {
    // Synthetic: a profile name with a colon in it.
    const QList<WifiSettings::SavedProfile> profiles = WifiSettings::parseWifiProfileList(
            QStringLiteral("Cafe\\: Upstairs:11111111-2222-3333-4444-555555555555:"
                           "802-11-wireless\n"
                           "Wired connection 1:1eb7b59f-356f-3501-b5ba-54f6f75444c1:"
                           "802-3-ethernet\n"));
    ASSERT_EQ(1, profiles.size());
    EXPECT_QSTRING_EQ(QStringLiteral("Cafe: Upstairs"), profiles.at(0).name);
    EXPECT_QSTRING_EQ(QStringLiteral("11111111-2222-3333-4444-555555555555"),
            profiles.at(0).uuid);
}

TEST(WifiSettingsTest, ParseSavedWifiNamesOfNothingIsEmpty) {
    // Synthetic: no profiles at all.
    EXPECT_TRUE(WifiSettings::parseWifiProfileList(QString()).isEmpty());
}

TEST(WifiSettingsTest, ParseWifiProfileListKeepsARepeatedUuidOnce) {
    // Synthetic: two profiles with one name are two profiles; one UUID listed
    // twice is one.
    const QList<WifiSettings::SavedProfile> profiles = WifiSettings::parseWifiProfileList(
            QStringLiteral("Home:aaaa:802-11-wireless\n"
                           "Home:bbbb:802-11-wireless\n"
                           "Home:aaaa:802-11-wireless\n"));
    ASSERT_EQ(2, profiles.size());
    EXPECT_QSTRING_EQ(QStringLiteral("aaaa"), profiles.at(0).uuid);
    EXPECT_QSTRING_EQ(QStringLiteral("bbbb"), profiles.at(1).uuid);
}

// --- parseSavedWifiProfiles -------------------------------------------------

TEST(WifiSettingsTest, ParseSavedWifiProfilesReadsTheMeasuredBlocks) {
    // Measured: the Wi-Fi profile is read whole, and the ethernet block,
    // which has no SSID line, is not a Wi-Fi profile at all.
    const QList<WifiSettings::SavedProfile> profiles =
            WifiSettings::parseSavedWifiProfiles(kProfileDetails);
    ASSERT_EQ(1, profiles.size());
    EXPECT_QSTRING_EQ(QStringLiteral("iwanttoridemybicycle"), profiles.at(0).name);
    EXPECT_QSTRING_EQ(kHomeUuid, profiles.at(0).uuid);
    EXPECT_QSTRING_EQ(QStringLiteral("iwanttoridemybicycle"), profiles.at(0).ssid);
}

TEST(WifiSettingsTest, ParseSavedWifiProfilesKeysOnTheUuidNotThePosition) {
    // Synthetic, in the measured shape: a profile whose name is not its SSID,
    // a second profile for the same SSID, the first uuid asked for twice
    // (nmcli then prints its block twice, measured), and a block whose lines
    // come in another order. Each is read by the uuid it names.
    const QList<WifiSettings::SavedProfile> profiles = WifiSettings::parseSavedWifiProfiles(
            QStringLiteral("connection.id:preconfigured\n"
                           "connection.uuid:pppp\n"
                           "802-11-wireless.ssid:Home\n"
                           "\n"
                           "802-11-wireless.ssid:Home\n"
                           "connection.uuid:hhhh\n"
                           "connection.id:Home backup\n"
                           "\n"
                           "connection.id:preconfigured\n"
                           "connection.uuid:pppp\n"
                           "802-11-wireless.ssid:Home\n"));
    ASSERT_EQ(2, profiles.size());
    EXPECT_QSTRING_EQ(QStringLiteral("preconfigured"), profiles.at(0).name);
    EXPECT_QSTRING_EQ(QStringLiteral("pppp"), profiles.at(0).uuid);
    EXPECT_QSTRING_EQ(QStringLiteral("Home"), profiles.at(0).ssid);
    EXPECT_QSTRING_EQ(QStringLiteral("Home backup"), profiles.at(1).name);
    EXPECT_QSTRING_EQ(QStringLiteral("hhhh"), profiles.at(1).uuid);
    EXPECT_QSTRING_EQ(QStringLiteral("Home"), profiles.at(1).ssid);
}

TEST(WifiSettingsTest, ParseSavedWifiProfilesToleratesCrlfEscapesAndUnknownKeys) {
    // Synthetic: CRLF endings, a key this class does not ask for, and a name
    // and an SSID holding both terse escapes.
    const QList<WifiSettings::SavedProfile> profiles = WifiSettings::parseSavedWifiProfiles(
            QStringLiteral("connection.id:Cafe\\: Upstairs\r\n"
                           "connection.uuid:cccc\r\n"
                           "connection.type:802-11-wireless\r\n"
                           "802-11-wireless.ssid:a\\:b\\\\c\r\n"));
    ASSERT_EQ(1, profiles.size());
    EXPECT_QSTRING_EQ(QStringLiteral("Cafe: Upstairs"), profiles.at(0).name);
    EXPECT_QSTRING_EQ(QStringLiteral("cccc"), profiles.at(0).uuid);
    EXPECT_QSTRING_EQ(QStringLiteral("a:b\\c"), profiles.at(0).ssid);
}

TEST(WifiSettingsTest, ParseSavedWifiProfilesSplitsBlocksWithNoSeparator) {
    // Synthetic: a repeated key starts a new block, so two profiles printed
    // without the empty line between them are never merged into one.
    const QList<WifiSettings::SavedProfile> profiles = WifiSettings::parseSavedWifiProfiles(
            QStringLiteral("connection.id:One\n"
                           "connection.uuid:1111\n"
                           "802-11-wireless.ssid:NetOne\n"
                           "connection.id:Two\n"
                           "connection.uuid:2222\n"
                           "802-11-wireless.ssid:NetTwo\n"));
    ASSERT_EQ(2, profiles.size());
    EXPECT_QSTRING_EQ(QStringLiteral("NetOne"), profiles.at(0).ssid);
    EXPECT_QSTRING_EQ(QStringLiteral("2222"), profiles.at(1).uuid);
    EXPECT_QSTRING_EQ(QStringLiteral("NetTwo"), profiles.at(1).ssid);
}

TEST(WifiSettingsTest, ParseSavedWifiProfilesOfNothingIsEmpty) {
    // Synthetic: empty output, and a block with no uuid.
    EXPECT_TRUE(WifiSettings::parseSavedWifiProfiles(QString()).isEmpty());
    EXPECT_TRUE(WifiSettings::parseSavedWifiProfiles(
            QStringLiteral("connection.id:Home\n802-11-wireless.ssid:Home\n"))
                        .isEmpty());
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

TEST(WifiSettingsTest, ParseIpv4ReadsTheCombinedStatusRead) {
    // Measured: the address alongside the connection's UUID.
    EXPECT_EQ(QStringLiteral("192.168.4.39"), WifiSettings::parseIpv4(kDeviceShowWlan0));
}

// --- parseConnectionUuid ----------------------------------------------------

TEST(WifiSettingsTest, ParseConnectionUuidReadsTheMeasuredLine) {
    // Measured.
    EXPECT_QSTRING_EQ(kHomeUuid, WifiSettings::parseConnectionUuid(kDeviceShowWlan0));
}

TEST(WifiSettingsTest, ParseConnectionUuidKeysOnTheFieldName) {
    // Synthetic: the same two lines in the other order, CRLF, and a second
    // address; the UUID is found by its name wherever it is.
    const QString output = QStringLiteral(
            "GENERAL.CON-UUID:a0b1e9f8-84e5-4e83-9837-6ca90bdc30ce\r\n"
            "IP4.ADDRESS[1]:10.0.0.5/24\r\n"
            "IP4.ADDRESS[2]:10.0.0.6/24\r\n");
    EXPECT_QSTRING_EQ(kHomeUuid, WifiSettings::parseConnectionUuid(output));
    EXPECT_EQ(QStringLiteral("10.0.0.5"), WifiSettings::parseIpv4(output));
}

TEST(WifiSettingsTest, ParseConnectionUuidOfNothingIsEmpty) {
    // Synthetic: no output, and an address with no UUID line.
    EXPECT_TRUE(WifiSettings::parseConnectionUuid(QString()).isEmpty());
    EXPECT_TRUE(WifiSettings::parseConnectionUuid(kIpv4Wlan0).isEmpty());
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

// The argv prefixes the state tests script, each exactly as WifiSettings
// builds it.
const QStringList kStatusArgs{"-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status"};
const QStringList kRadioArgs{"-t", "-f", "WIFI", "general"};
const QStringList kIpv4Args{"-t", "-f", "IP4.ADDRESS,GENERAL.CON-UUID", "device", "show"};
const QStringList kSnapshotArgs{"-t", "-f", "NAME,TYPE,DEVICE", "connection", "show"};
const QStringList kScanProfilesArgs{"-t", "-f", "NAME,UUID,TYPE", "connection", "show"};
const QStringList kScanSsidsArgs{
        "-t", "-f", "connection.id,connection.uuid,802-11-wireless.ssid", "connection", "show"};
const QStringList kScanListArgs{
        "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list"};
// Every join, cancel clean-up, disconnect and forget starts with --wait. The
// tests allow them all and then assert on exactly which one ran.
const QStringList kActionArgs{"--wait"};

// An argv as one string that keeps its element boundaries, for comparing and
// for failure messages: [connection] [up] [uuid] [x].
QString argvString(const QStringList& args) {
    if (args.isEmpty()) {
        return QStringLiteral("(none)");
    }
    return QStringLiteral("[") + args.join(QStringLiteral("] [")) + QStringLiteral("]");
}

bool startsWith(const QStringList& args, const QStringList& prefix) {
    return args.size() >= prefix.size() && args.mid(0, prefix.size()) == prefix;
}

} // namespace

// The state-machine half. Three rules keep a real WifiSettings safe in a
// harness that has no NetworkManager, and nothing below may break any of them:
//
//  - The event loop is never turned. The constructor schedules its one status
//    read with QTimer::singleShot(0, ...), so no processEvents(), no
//    QSignalSpy::wait() and no exec() anywhere here, and that read never fires.
//    Nor, for the same reason, does an op's watchdog.
//  - Every nmcli call goes to the fake this fixture installs in SetUp() as
//    WifiSettings::s_pNmcliFake, so no test in the binary can reach a real
//    nmcli, and WifiSettings never constructs a QProcess while it is set.
//  - The fake fails closed. A synchronous read answers only an argv prefix the
//    test scripted, and an async op starts only for a prefix the test said to
//    expect; anything else is ADD_FAILURE() with the argv printed, and the
//    call fails as if nmcli could not be run. An async op that did start
//    stays in flight until the test ends it with succeedOp() or failOp().
//
// WifiSettings names this fixture a friend, which is what lets the rows a tap
// acts on be seeded without a scan, the page and selected index be read back
// without a skin, and the fake stand in for nmcli. The TEST_F bodies are
// subclasses and not friends, so everything private goes through a helper
// here.
class WifiSettingsStateTest : public MixxxTest, public WifiSettings::NmcliFake {
  protected:
    void SetUp() override {
        MixxxTest::SetUp();
        ASSERT_EQ(nullptr, WifiSettings::s_pNmcliFake) << "a previous test left its fake installed";
        WifiSettings::s_pNmcliFake = this;
        ASSERT_NE(nullptr, WifiSettings::s_pNmcliFake);
    }
    void TearDown() override {
        WifiSettings::s_pNmcliFake = nullptr;
        MixxxTest::TearDown();
    }

    // --- The fake ---------------------------------------------------------

    bool readSync(const QStringList& args,
            int timeoutMs,
            QString* pOut,
            QString* pError) override {
        Q_UNUSED(timeoutMs);
        m_syncCalls.append(args);
        // The most recent script for a prefix wins, so a test changes what
        // the box reports by scripting the same read again.
        for (qsizetype i = m_syncScript.size() - 1; i >= 0; --i) {
            const SyncReply& reply = m_syncScript.at(i);
            if (!startsWith(args, reply.prefix)) {
                continue;
            }
            if (reply.ok) {
                if (pOut) {
                    *pOut = reply.out;
                }
                return true;
            }
            if (pError) {
                *pError = reply.err;
            }
            return false;
        }
        ADD_FAILURE() << "unscripted synchronous nmcli call: "
                      << argvString(args).toStdString();
        if (pError) {
            *pError = QStringLiteral("not scripted");
        }
        return false;
    }

    bool startAsync(const QStringList& args) override {
        m_asyncCalls.append(args);
        for (const QStringList& prefix : std::as_const(m_asyncExpected)) {
            if (startsWith(args, prefix)) {
                return true;
            }
        }
        ADD_FAILURE() << "unscripted async nmcli call: " << argvString(args).toStdString();
        return false;
    }

    // Scripts a synchronous read of any argv starting with prefix.
    void scriptSync(const QStringList& prefix, const QString& out) {
        m_syncScript.append(SyncReply{prefix, true, out, QString()});
    }
    void scriptSyncFailure(const QStringList& prefix, const QString& err) {
        m_syncScript.append(SyncReply{prefix, false, QString(), err});
    }
    // Lets async ops whose argv starts with prefix start.
    void expectAsync(const QStringList& prefix) {
        m_asyncExpected.append(prefix);
    }
    // Every async argv the code issued, in order, refused ones included.
    const QList<QStringList>& asyncCalls() const {
        return m_asyncCalls;
    }
    const QList<QStringList>& syncCalls() const {
        return m_syncCalls;
    }
    QString lastAsync() const {
        return m_asyncCalls.isEmpty() ? argvString(QStringList())
                                      : argvString(m_asyncCalls.last());
    }
    // Whether any async argv from index from on has word as one element.
    bool anyAsyncHas(const QString& word, qsizetype from = 0) const {
        for (qsizetype i = from; i < m_asyncCalls.size(); ++i) {
            if (m_asyncCalls.at(i).contains(word)) {
                return true;
            }
        }
        return false;
    }

    // The status refresh's reads, answered as a box on wlan0 connected by
    // profile (with that connection's uuid, when one is given), or on
    // nothing.
    void scriptConnected(const QString& profile, const QString& uuid = QString()) {
        scriptSync(kStatusArgs,
                QStringLiteral("wlan0:wifi:connected:") + profile +
                        QStringLiteral("\n"
                                       "p2p-dev-wlan0:wifi-p2p:disconnected:\n"));
        scriptSync(kRadioArgs, QStringLiteral("enabled\n"));
        QString deviceShow = kIpv4Wlan0;
        if (!uuid.isEmpty()) {
            deviceShow.append(QStringLiteral("GENERAL.CON-UUID:"));
            deviceShow.append(uuid);
            deviceShow.append(QLatin1Char('\n'));
        }
        scriptSync(kIpv4Args, deviceShow);
    }
    void scriptDisconnected() {
        scriptSync(kStatusArgs, QStringLiteral("wlan0:wifi:disconnected:\n"));
        scriptSync(kRadioArgs, QStringLiteral("enabled\n"));
    }

    // Ends the async op in flight the way a real nmcli would have.
    static void succeedOp(WifiSettings* pSettings, const QString& out = QString()) {
        WifiSettings::NmcliResult result;
        result.started = true;
        result.normalExit = true;
        result.exitCode = 0;
        result.out = out;
        endFakeOp(pSettings, result);
    }
    static void failOp(WifiSettings* pSettings, int exitCode, const QString& err) {
        WifiSettings::NmcliResult result;
        result.started = true;
        result.normalExit = true;
        result.exitCode = exitCode;
        result.err = err;
        endFakeOp(pSettings, result);
    }
    static bool opInFlight(const WifiSettings* pSettings) {
        return pSettings->m_opInFlight;
    }
    bool fakeIsInstalled() const {
        return WifiSettings::s_pNmcliFake == static_cast<const WifiSettings::NmcliFake*>(this);
    }

    // --- Driving the instance ----------------------------------------------

    std::unique_ptr<WifiSettings> newSettings() {
        return std::make_unique<WifiSettings>(config());
    }
    static void refresh(WifiSettings* pSettings) {
        pSettings->refreshStatus();
    }
    static void applyStatusOf(WifiSettings* pSettings,
            int state,
            const QString& connection = QString(),
            const QString& uuid = QString()) {
        pSettings->applyStatus(state, connection, QString(), uuid);
    }
    // A join snapshot left over from somewhere, which no real path leaves
    // (every way a join ends consumes it); for pinning the defensive reset.
    static void seedStaleSnapshot(WifiSettings* pSettings, const QStringList& names) {
        pSettings->m_joinSnapshot = names;
    }
    static bool startScanOf(WifiSettings* pSettings) {
        return pSettings->startScan(false);
    }
    static QString describeFailureOf(const WifiSettings* pSettings, const QString& err) {
        WifiSettings::NmcliResult result;
        result.started = true;
        result.normalExit = true;
        result.exitCode = 4;
        result.err = err;
        return pSettings->describeFailure(result);
    }

    // A whole scan through the fake: the profile list, their SSIDs (only when
    // the list has a Wi-Fi profile, as the code does), then the network list.
    void runScan(WifiSettings* pSettings,
            const QString& profileList,
            const QString& profileDetails,
            const QString& wifiList) {
        expectAsync(kScanProfilesArgs);
        expectAsync(kScanSsidsArgs);
        expectAsync(kScanListArgs);
        ASSERT_TRUE(startScanOf(pSettings));
        succeedOp(pSettings, profileList);
        if (!WifiSettings::parseWifiProfileList(profileList).isEmpty()) {
            succeedOp(pSettings, profileDetails);
        }
        succeedOp(pSettings, wifiList);
        ASSERT_FALSE(opInFlight(pSettings));
    }

    static int rowIndex(const WifiSettings* pSettings, const QString& ssid) {
        const QList<WifiRow> rows = pSettings->rows();
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i).ssid == ssid) {
                return i;
            }
        }
        ADD_FAILURE() << "no row " << ssid.toStdString();
        return -1;
    }
    static WifiRow rowNamed(const WifiSettings* pSettings, const QString& ssid) {
        const int index = rowIndex(pSettings, ssid);
        return index >= 0 ? pSettings->rows().at(index) : WifiRow();
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

    // The way into the rows that does not run a scan: publishRows() is what a
    // landed scan calls, with the connection the rows were seen on -- empty
    // here, matching the disconnected state a fresh instance starts in.
    static void seedRows(WifiSettings* pSettings, const QList<WifiRow>& rows) {
        pSettings->publishRows(rows, QString());
    }
    // The same, for rows scanned while on connection.
    static void seedRowsOn(
            WifiSettings* pSettings, const QList<WifiRow>& rows, const QString& connection) {
        pSettings->publishRows(rows, connection);
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

    // Two open networks nothing has saved: a tap on either joins at once
    // with `device wifi connect`, no keypad.
    static QList<WifiRow> twoOpenNetworks() {
        QList<WifiRow> rows;
        WifiRow guest;
        guest.ssid = QStringLiteral("CafeGuest");
        guest.signalPercent = 60;
        rows.append(guest);
        WifiRow library;
        library.ssid = QStringLiteral("Library");
        library.signalPercent = 50;
        rows.append(library);
        return rows;
    }

  private:
    struct SyncReply {
        QStringList prefix;
        bool ok;
        QString out;
        QString err;
    };

    static void endFakeOp(WifiSettings* pSettings, const WifiSettings::NmcliResult& result) {
        if (!pSettings->m_opInFlight) {
            ADD_FAILURE() << "no nmcli op in flight to end";
            return;
        }
        if (pSettings->m_pOpProcess) {
            ADD_FAILURE() << "a real nmcli process is in flight";
            return;
        }
        pSettings->finishOp(result);
    }

    QList<SyncReply> m_syncScript;
    QList<QStringList> m_asyncExpected;
    QList<QStringList> m_syncCalls;
    QList<QStringList> m_asyncCalls;
};

TEST_F(WifiSettingsStateTest, TheNmcliFakeIsInstalled) {
    // Every other test here relies on this: with it, no nmcli is ever run.
    EXPECT_TRUE(fakeIsInstalled());
}

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
    // Seven characters, one short: the eighth would make this submit a legal
    // passphrase and start a join, which the fake would report as an
    // unscripted call.
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
    // A legal length, so the password check passes and the missing target is
    // the only thing left to refuse. Without that guard, this starts a join:
    // its snapshot read is unscripted (a failure from the fake) and the page
    // moves to 2.
    type(pSettings.get(), QStringLiteral("password123"));
    ASSERT_EQ(11, pSettings->passwordLength());

    pSettings->submitJoin();

    EXPECT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
    EXPECT_EQ(11, pSettings->passwordLength());
    EXPECT_TRUE(syncCalls().isEmpty());
    EXPECT_TRUE(asyncCalls().isEmpty());
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
    for (const double value : {-1.0, -0.5, 2.0, 1.0e9}) {
        writeCo("selected_index", value);
        EXPECT_EQ(-1, selectedIndexOf(pSettings.get())) << value;
    }
    // NaN from an in-range value, and checked to have landed: a write the CO
    // dropped would leave 1.0 there, and the -1 below would then be proving
    // nothing about NaN.
    writeCo("selected_index", 1.0);
    ASSERT_EQ(1, selectedIndexOf(pSettings.get()));
    writeCo("selected_index", nan);
    ASSERT_TRUE(util_isnan(readCo("selected_index")) != 0);
    EXPECT_EQ(-1, selectedIndexOf(pSettings.get()));
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
    for (const double value : {-1.0, 4.0, 99.0, 1.0e9}) {
        writeCo("page", value);
        EXPECT_EQ(-1, pageOf(pSettings.get())) << value;
    }
    // NaN from an in-range value, and checked to have landed, as for
    // selected_index.
    writeCo("page", static_cast<double>(WifiSettings::kPagePassword));
    ASSERT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    writeCo("page", nan);
    ASSERT_TRUE(util_isnan(readCo("page")) != 0);
    EXPECT_EQ(-1, pageOf(pSettings.get()));

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

// --- The fake itself ---------------------------------------------------------

TEST_F(WifiSettingsStateTest, AnUnscriptedNmcliCallFailsTheTestAndFailsClosed) {
    const auto pSettings = newSettings();

    // A synchronous read nobody scripted is reported, and it fails, so the
    // refresh reads it as NetworkManager not answering.
    EXPECT_NONFATAL_FAILURE(refresh(pSettings.get()), "DEVICE,TYPE,STATE,CONNECTION");
    EXPECT_EQ(WifiSettings::kStateNotResponding, pSettings->state());

    // An async op nobody said to expect is reported, and ends at once as one
    // that never started, leaving the slot free.
    scriptDisconnected();
    refresh(pSettings.get());
    ASSERT_EQ(WifiSettings::kStateDisconnected, pSettings->state());
    bool started = false;
    EXPECT_NONFATAL_FAILURE(started = startScanOf(pSettings.get()), "NAME,UUID,TYPE");
    EXPECT_TRUE(started);
    EXPECT_FALSE(opInFlight(pSettings.get()));
    EXPECT_EQ(0.0, readCo("scan"));
    EXPECT_TRUE(pSettings->rows().isEmpty());
}

// --- Saved networks are matched by SSID ---------------------------------------

TEST_F(WifiSettingsStateTest, AScanReadsEverySavedProfilesSsidByUuid) {
    const auto pSettings = newSettings();
    // Synthetic, in the measured shape: one profile named unlike its SSID,
    // an ethernet profile, and a second Wi-Fi profile; and a network that
    // happens to be called what the first profile is called.
    runScan(pSettings.get(),
            QStringLiteral("preconfigured:pppp:802-11-wireless\n"
                           "Wired connection 1:1eb7b59f-356f-3501-b5ba-54f6f75444c1:"
                           "802-3-ethernet\n"
                           "Cafe:cccc:802-11-wireless\n"),
            QStringLiteral("connection.id:preconfigured\n"
                           "connection.uuid:pppp\n"
                           "802-11-wireless.ssid:Home\n"
                           "\n"
                           "connection.id:Cafe\n"
                           "connection.uuid:cccc\n"
                           "802-11-wireless.ssid:CafeNet\n"),
            QStringLiteral(" :Home:70:WPA2\n"
                           " :preconfigured:60:WPA2\n"
                           " :CafeNet:50:WPA2\n"));

    // Only the Wi-Fi profiles, each uuid its own argv element after its own
    // "uuid", in one call.
    ASSERT_EQ(3, asyncCalls().size());
    EXPECT_QSTRING_EQ(argvString(QStringList{"-t",
                              "-f",
                              "connection.id,connection.uuid,802-11-wireless.ssid",
                              "connection",
                              "show",
                              "uuid",
                              "pppp",
                              "uuid",
                              "cccc"}),
            argvString(asyncCalls().at(1)));
    EXPECT_TRUE(rowNamed(pSettings.get(), QStringLiteral("Home")).saved);
    EXPECT_TRUE(rowNamed(pSettings.get(), QStringLiteral("CafeNet")).saved);
    // Named like a profile, but no profile joins it.
    EXPECT_FALSE(rowNamed(pSettings.get(), QStringLiteral("preconfigured")).saved);
}

TEST_F(WifiSettingsStateTest, AScanWithNoSavedWifiProfilesSkipsTheSsidRead) {
    const auto pSettings = newSettings();
    const QString homeSaved = QStringLiteral(
            "connection.id:Home\n"
            "connection.uuid:hhhh\n"
            "802-11-wireless.ssid:Home\n");
    runScan(pSettings.get(),
            QStringLiteral("Home:hhhh:802-11-wireless\n"),
            homeSaved,
            QStringLiteral(" :Home:70:WPA2\n"));
    ASSERT_TRUE(rowNamed(pSettings.get(), QStringLiteral("Home")).saved);
    const qsizetype before = asyncCalls().size();

    // Measured listing, minus its Wi-Fi profile: nothing Wi-Fi is saved now.
    runScan(pSettings.get(),
            QStringLiteral("lo:bcb61ff9-40e5-4c9c-af54-68210da7f772:loopback\n"
                           "Wired connection 1:1eb7b59f-356f-3501-b5ba-54f6f75444c1:"
                           "802-3-ethernet\n"),
            QString(),
            QStringLiteral(" :Home:70:WPA2\n"));

    // The profile list, then straight to the network list.
    ASSERT_EQ(before + 2, asyncCalls().size());
    EXPECT_TRUE(startsWith(asyncCalls().at(before), kScanProfilesArgs));
    EXPECT_TRUE(startsWith(asyncCalls().at(before + 1), kScanListArgs));
    // And the profile that went is no longer what makes the row saved.
    EXPECT_FALSE(rowNamed(pSettings.get(), QStringLiteral("Home")).saved);
}

TEST_F(WifiSettingsStateTest, TappingASavedNetworkBringsItsProfileUpByUuid) {
    const auto pSettings = newSettings();
    scriptDisconnected();
    refresh(pSettings.get());
    // The imager's profile: called "preconfigured", joining Home.
    runScan(pSettings.get(),
            QStringLiteral("preconfigured:pppp:802-11-wireless\n"),
            QStringLiteral("connection.id:preconfigured\n"
                           "connection.uuid:pppp\n"
                           "802-11-wireless.ssid:Home\n"),
            QStringLiteral(" :Home:70:WPA2\n"));
    const WifiRow home = rowNamed(pSettings.get(), QStringLiteral("Home"));
    ASSERT_TRUE(home.saved);
    ASSERT_TRUE(home.secured);
    scriptSync(kSnapshotArgs, QStringLiteral("preconfigured:802-11-wireless:\n"));
    expectAsync(kActionArgs);
    const qsizetype before = asyncCalls().size();

    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Home")));

    // Joining at once through the saved profile, not the keypad a secured
    // network nothing has saved would get.
    EXPECT_EQ(WifiSettings::kPageJoining, pageOf(pSettings.get()));
    EXPECT_QSTRING_EQ(QStringLiteral("Home"), pSettings->joinTarget());
    ASSERT_EQ(before + 1, asyncCalls().size());
    EXPECT_QSTRING_EQ(argvString(QStringList{"--wait", "30", "connection", "up", "uuid", "pppp"}),
            lastAsync());
}

TEST_F(WifiSettingsStateTest, AProfileNamedLikeAnotherNetworkDoesNotMakeThatNetworkActive) {
    const auto pSettings = newSettings();
    // Synthetic: the profile "Cafe" joins the network Home, and a network
    // called Cafe is also in range.
    runScan(pSettings.get(),
            QStringLiteral("Cafe:cccc:802-11-wireless\n"),
            QStringLiteral("connection.id:Cafe\n"
                           "connection.uuid:cccc\n"
                           "802-11-wireless.ssid:Home\n"),
            QStringLiteral(" :Home:70:WPA2\n"
                           " :Cafe:60:WPA2\n"));
    // Now on profile "Cafe". The rows were scanned on no connection, so their
    // active flags do not count, and only the lookup by SSID decides.
    applyStatusOf(pSettings.get(), WifiSettings::kStateConnected, QStringLiteral("Cafe"));

    EXPECT_TRUE(pSettings->isActiveNow(rowNamed(pSettings.get(), QStringLiteral("Home"))));
    EXPECT_FALSE(pSettings->isActiveNow(rowNamed(pSettings.get(), QStringLiteral("Cafe"))));
    EXPECT_QSTRING_EQ(QStringLiteral("Connected to Home"), pSettings->statusLine());

    // A tap on Cafe is a tap on a network the box is not on: the keypad, not
    // the manage page whose Disconnect would drop Home.
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Cafe")));
    EXPECT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    EXPECT_QSTRING_EQ(QStringLiteral("Cafe"), pSettings->joinTarget());
}

// --- A cancelled or failed join ----------------------------------------------

TEST_F(WifiSettingsStateTest, CancellingAJoinNeverTakesDownTheProfileTheBoxWasOn) {
    const auto pSettings = newSettings();
    scriptDisconnected();
    refresh(pSettings.get());
    runScan(pSettings.get(),
            QStringLiteral("preconfigured:pppp:802-11-wireless\n"),
            QStringLiteral("connection.id:preconfigured\n"
                           "connection.uuid:pppp\n"
                           "802-11-wireless.ssid:Home\n"),
            QStringLiteral(" :Home:70:WPA2\n"));
    // The last status read said disconnected, but the box has since got back
    // onto preconfigured by itself, and the snapshot, which is fresher, says
    // so. Matched by the profile's name, not by Home.
    scriptSync(kSnapshotArgs, QStringLiteral("preconfigured:802-11-wireless:wlan0\n"));
    expectAsync(kActionArgs);
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Home")));
    ASSERT_EQ(WifiSettings::kPageJoining, pageOf(pSettings.get()));
    ASSERT_QSTRING_EQ(
            argvString(QStringList{"--wait", "30", "connection", "up", "uuid", "pppp"}),
            lastAsync());
    const qsizetype joinCall = asyncCalls().size() - 1;

    pSettings->cancelJoin();

    // `connection down` would block its autoconnect and strand the box.
    EXPECT_FALSE(anyAsyncHas(QStringLiteral("down"), joinCall)) << lastAsync().toStdString();
    EXPECT_FALSE(anyAsyncHas(QStringLiteral("delete"), joinCall)) << lastAsync().toStdString();
    EXPECT_EQ(joinCall + 1, asyncCalls().size());
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
}

TEST_F(WifiSettingsStateTest, CancellingAJoinTakesDownTheActivationItStarted) {
    // The other side of the test above, so it cannot pass by never taking
    // anything down: the box was on another profile, so the one this join
    // brought up is stopped, by its UUID.
    const auto pSettings = newSettings();
    scriptDisconnected();
    refresh(pSettings.get());
    runScan(pSettings.get(),
            QStringLiteral("preconfigured:pppp:802-11-wireless\n"),
            QStringLiteral("connection.id:preconfigured\n"
                           "connection.uuid:pppp\n"
                           "802-11-wireless.ssid:Home\n"),
            QStringLiteral(" :Home:70:WPA2\n"));
    scriptSync(kSnapshotArgs,
            QStringLiteral("preconfigured:802-11-wireless:\n"
                           "Other:802-11-wireless:wlan0\n"));
    expectAsync(kActionArgs);
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Home")));
    ASSERT_EQ(WifiSettings::kPageJoining, pageOf(pSettings.get()));

    pSettings->cancelJoin();

    EXPECT_QSTRING_EQ(
            argvString(QStringList{"--wait", "10", "connection", "down", "uuid", "pppp"}),
            lastAsync());
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
}

TEST_F(WifiSettingsStateTest, CancellingAJoinTakesNothingDownWhenTheSnapshotReadFailed) {
    const auto pSettings = newSettings();
    scriptDisconnected();
    refresh(pSettings.get());
    seedRows(pSettings.get(), twoOpenNetworks());
    expectAsync(kActionArgs);
    expectAsync(kScanProfilesArgs);

    // A first join whose snapshot read works and finds the box on nothing,
    // so it leaves a start profile behind that is known, and empty.
    scriptSync(kSnapshotArgs, QStringLiteral("Wired connection 1:802-3-ethernet:\n"));
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("CafeGuest")));
    ASSERT_QSTRING_EQ(argvString(QStringList{"--wait",
                              "30",
                              "device",
                              "wifi",
                              "connect",
                              "CafeGuest",
                              "ifname",
                              "wlan0"}),
            lastAsync());
    succeedOp(pSettings.get());
    // Its success starts a scan, which the next tap pre-empts.
    ASSERT_TRUE(opInFlight(pSettings.get()));

    // The second join's snapshot read fails, so the profile the box is on is
    // unknown, and what the first join learnt must not stand in for it.
    scriptSyncFailure(kSnapshotArgs, QStringLiteral("Error: NetworkManager is not running."));
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Library")));
    ASSERT_EQ(WifiSettings::kPageJoining, pageOf(pSettings.get()));
    ASSERT_QSTRING_EQ(argvString(QStringList{"--wait",
                              "30",
                              "device",
                              "wifi",
                              "connect",
                              "Library",
                              "ifname",
                              "wlan0"}),
            lastAsync());
    const qsizetype joinCall = asyncCalls().size() - 1;

    pSettings->cancelJoin();

    EXPECT_EQ(joinCall + 1, asyncCalls().size()) << lastAsync().toStdString();
    EXPECT_FALSE(anyAsyncHas(QStringLiteral("down"), joinCall));
    EXPECT_FALSE(anyAsyncHas(QStringLiteral("delete"), joinCall));
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
}

TEST_F(WifiSettingsStateTest, AFailedJoinNeverDeletesAProfileThatWasThereBefore) {
    const auto pSettings = newSettings();
    scriptDisconnected();
    refresh(pSettings.get());
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Alpha")));
    type(pSettings.get(), QStringLiteral("password1"));
    // A profile called Alpha exists already, of another type entirely.
    // `connection delete id Alpha` would take it.
    scriptSync(kSnapshotArgs,
            QStringLiteral("Alpha:802-3-ethernet:\n"
                           "Wired connection 1:802-3-ethernet:\n"));
    expectAsync(kActionArgs);
    pSettings->submitJoin();
    ASSERT_QSTRING_EQ(argvString(QStringList{"--wait",
                              "30",
                              "device",
                              "wifi",
                              "connect",
                              "Alpha",
                              "password",
                              "password1",
                              "ifname",
                              "wlan0"}),
            lastAsync());
    const qsizetype joinCall = asyncCalls().size() - 1;

    failOp(pSettings.get(),
            4,
            QStringLiteral("Error: Connection activation failed: (53) The Wi-Fi network "
                           "could not be found."));

    EXPECT_FALSE(anyAsyncHas(QStringLiteral("delete"), joinCall)) << lastAsync().toStdString();
    EXPECT_EQ(joinCall + 1, asyncCalls().size());
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
}

TEST_F(WifiSettingsStateTest, AFailedJoinDeletesTheProfileItCreated) {
    const auto pSettings = newSettings();
    scriptDisconnected();
    refresh(pSettings.get());
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Alpha")));
    type(pSettings.get(), QStringLiteral("password1"));
    // No Alpha before the attempt, so one after it is the attempt's.
    scriptSync(kSnapshotArgs, QStringLiteral("Wired connection 1:802-3-ethernet:\n"));
    expectAsync(kActionArgs);
    pSettings->submitJoin();
    const qsizetype joinCall = asyncCalls().size() - 1;

    failOp(pSettings.get(),
            4,
            QStringLiteral("Error: Connection activation failed: (53) The Wi-Fi network "
                           "could not be found."));

    // Exactly one delete, of exactly that name.
    ASSERT_EQ(joinCall + 2, asyncCalls().size());
    EXPECT_QSTRING_EQ(
            argvString(QStringList{"--wait", "10", "connection", "delete", "id", "Alpha"}),
            lastAsync());
}

TEST_F(WifiSettingsStateTest, DescribeFailureMasksThePassword) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(0);
    // Runs of spaces on purpose: masking after simplifying would no longer
    // find it.
    const QString password = QStringLiteral("pass  word  12");
    type(pSettings.get(), password);
    ASSERT_EQ(password.size(), pSettings->passwordLength());

    const QString text = describeFailureOf(pSettings.get(),
            QStringLiteral("Error: 802-11-wireless-security.psk: 'pass  word  12' is invalid"));

    EXPECT_FALSE(text.contains(password)) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("pass word 12"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("********"))) << text.toStdString();
}

// --- Page 3 actions -------------------------------------------------------------

TEST_F(WifiSettingsStateTest, DisconnectRefusesWhenTheBoxHasMovedToAnotherNetwork) {
    const auto pSettings = newSettings();
    scriptConnected(QStringLiteral("Home"));
    refresh(pSettings.get());
    ASSERT_EQ(WifiSettings::kStateConnected, pSettings->state());
    runScan(pSettings.get(),
            QStringLiteral("Home:hhhh:802-11-wireless\n"),
            QStringLiteral("connection.id:Home\n"
                           "connection.uuid:hhhh\n"
                           "802-11-wireless.ssid:Home\n"),
            QStringLiteral("*:Home:70:WPA2\n"
                           " :Other:60:WPA2\n"));
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Home")));
    ASSERT_EQ(WifiSettings::kPageManage, pageOf(pSettings.get()));
    expectAsync(kActionArgs);
    const qsizetype before = asyncCalls().size();

    // Behind the DJ's back, the box has moved to another network, and page 3
    // still says Home. The refresh's own connection-change guard in
    // applyStatus() is what sends page 3 to the list here, before
    // manageTargetStillConnected() compares anything; the SSID comparison
    // itself is pinned by DisconnectRefusesForANetworkNamedLikeTheConnectedProfile.
    scriptConnected(QStringLiteral("Other"));
    writeCo("disconnect", 1.0);

    EXPECT_FALSE(anyAsyncHas(QStringLiteral("disconnect"), before)) << lastAsync().toStdString();
    EXPECT_EQ(before, asyncCalls().size());
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
    EXPECT_EQ(0.0, readCo("disconnect"));
}

TEST_F(WifiSettingsStateTest, ForgetDeletesTheConnectedProfileByUuidNotTheRowSsid) {
    const auto pSettings = newSettings();
    scriptConnected(QStringLiteral("preconfigured"), QStringLiteral("pppp"));
    refresh(pSettings.get());
    runScan(pSettings.get(),
            QStringLiteral("preconfigured:pppp:802-11-wireless\n"),
            QStringLiteral("connection.id:preconfigured\n"
                           "connection.uuid:pppp\n"
                           "802-11-wireless.ssid:Home\n"),
            QStringLiteral("*:Home:70:WPA2\n"));
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Home")));
    ASSERT_EQ(WifiSettings::kPageManage, pageOf(pSettings.get()));
    ASSERT_QSTRING_EQ(QStringLiteral("Home"), pSettings->joinTarget());
    expectAsync(kActionArgs);

    writeCo("forget", 1.0);

    EXPECT_QSTRING_EQ(
            argvString(QStringList{"--wait", "10", "connection", "delete", "uuid", "pppp"}),
            lastAsync());
    // Home is the network, not a profile, and never an argument.
    EXPECT_FALSE(anyAsyncHas(QStringLiteral("Home")));

    // The delete lands and leaves the box on nothing.
    scriptDisconnected();
    succeedOp(pSettings.get());
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_EQ(0.0, readCo("forget"));
}

TEST_F(WifiSettingsStateTest, ForgetDeletesOnlyTheConnectedOneOfTwoProfilesForANetwork) {
    const auto pSettings = newSettings();
    scriptConnected(QStringLiteral("preconfigured"));
    refresh(pSettings.get());
    runScan(pSettings.get(),
            QStringLiteral("preconfigured:pppp:802-11-wireless\n"
                           "Home backup:bbbb:802-11-wireless\n"),
            QStringLiteral("connection.id:preconfigured\n"
                           "connection.uuid:pppp\n"
                           "802-11-wireless.ssid:Home\n"
                           "\n"
                           "connection.id:Home backup\n"
                           "connection.uuid:bbbb\n"
                           "802-11-wireless.ssid:Home\n"),
            QStringLiteral("*:Home:70:WPA2\n"));
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Home")));
    ASSERT_EQ(WifiSettings::kPageManage, pageOf(pSettings.get()));
    expectAsync(kActionArgs);
    const qsizetype before = asyncCalls().size();

    writeCo("forget", 1.0);

    ASSERT_EQ(before + 1, asyncCalls().size());
    EXPECT_QSTRING_EQ(
            argvString(QStringList{"--wait", "10", "connection", "delete", "uuid", "pppp"}),
            lastAsync());
    // Only the Forget's calls: the scan's own SSID read names both uuids.
    EXPECT_FALSE(anyAsyncHas(QStringLiteral("bbbb"), before));
}

TEST_F(WifiSettingsStateTest, ALinkChangeWhileManagingReturnsToTheList) {
    const auto pSettings = newSettings();
    applyStatusOf(pSettings.get(), WifiSettings::kStateConnected, QStringLiteral("Home"));
    QList<WifiRow> rows;
    WifiRow home;
    home.ssid = QStringLiteral("Home");
    home.signalPercent = 70;
    home.secured = true;
    home.active = true;
    rows.append(home);
    seedRowsOn(pSettings.get(), rows, QStringLiteral("Home"));
    pSettings->activateRow(0);
    ASSERT_EQ(WifiSettings::kPageManage, pageOf(pSettings.get()));

    // A status read finds the box on another network: page 3's Disconnect
    // would now act on that one.
    applyStatusOf(pSettings.get(), WifiSettings::kStateConnected, QStringLiteral("Other"));

    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
    EXPECT_TRUE(syncCalls().isEmpty());
    EXPECT_TRUE(asyncCalls().isEmpty());
}

TEST_F(WifiSettingsStateTest, AManageActionOnAStalledServiceRunsNothing) {
    const auto pSettings = newSettings();
    scriptConnected(QStringLiteral("Home"));
    refresh(pSettings.get());
    QList<WifiRow> rows;
    WifiRow home;
    home.ssid = QStringLiteral("Home");
    home.signalPercent = 70;
    home.secured = true;
    home.active = true;
    rows.append(home);
    seedRowsOn(pSettings.get(), rows, QStringLiteral("Home"));
    pSettings->activateRow(0);
    ASSERT_EQ(WifiSettings::kPageManage, pageOf(pSettings.get()));
    expectAsync(kActionArgs);

    // The refresh before the action fails. What refuses is applyStatus()'s
    // non-usable branch, which sends page 3 to the list on state 4, and then
    // the page and state checks in manageTargetStillConnected(), not its
    // SSID comparison.
    scriptSyncFailure(kStatusArgs, QStringLiteral("Error: NetworkManager is not running."));
    writeCo("disconnect", 1.0);

    EXPECT_EQ(WifiSettings::kStateNotResponding, pSettings->state());
    EXPECT_TRUE(asyncCalls().isEmpty()) << lastAsync().toStdString();
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_EQ(0.0, readCo("disconnect"));
}

TEST_F(WifiSettingsStateTest, DisconnectRefusesForANetworkNamedLikeTheConnectedProfile) {
    // The connection does not change here, so nothing but the SSID
    // comparison in manageTargetStillConnected() stands between page 3 for
    // "Cafe" and a Disconnect of Home.
    const auto pSettings = newSettings();
    scriptConnected(QStringLiteral("Cafe"), QStringLiteral("cccc"));
    refresh(pSettings.get());
    runScan(pSettings.get(),
            QStringLiteral("Cafe:cccc:802-11-wireless\n"),
            QStringLiteral("connection.id:Cafe\n"
                           "connection.uuid:cccc\n"
                           "802-11-wireless.ssid:Home\n"),
            QStringLiteral("*:Home:70:WPA2\n"
                           " :Cafe:60:WPA2\n"));
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("Cafe")));
    ASSERT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    ASSERT_QSTRING_EQ(QStringLiteral("Cafe"), pSettings->joinTarget());
    expectAsync(kActionArgs);
    const qsizetype before = asyncCalls().size();

    // Page 3 reached from outside, the way the control socket or a skin
    // could, still naming Cafe.
    writeCo("page", static_cast<double>(WifiSettings::kPageManage));
    writeCo("disconnect", 1.0);

    EXPECT_FALSE(anyAsyncHas(QStringLiteral("disconnect"), before)) << lastAsync().toStdString();
    EXPECT_EQ(before, asyncCalls().size());
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_EQ(0.0, readCo("disconnect"));
}

// --- Two saved profiles with one name --------------------------------------------

TEST_F(WifiSettingsStateTest, TheActiveProfileIsResolvedByUuidWhenTwoShareAName) {
    const auto pSettings = newSettings();
    // Synthetic: two profiles called Home, joining two different networks.
    runScan(pSettings.get(),
            QStringLiteral("Home:aaaa:802-11-wireless\n"
                           "Home:bbbb:802-11-wireless\n"),
            QStringLiteral("connection.id:Home\n"
                           "connection.uuid:aaaa\n"
                           "802-11-wireless.ssid:Home\n"
                           "\n"
                           "connection.id:Home\n"
                           "connection.uuid:bbbb\n"
                           "802-11-wireless.ssid:HomeOld\n"),
            QStringLiteral(" :Home:70:WPA2\n"
                           " :HomeOld:60:WPA2\n"));
    // On the second of them. The rows were scanned on no connection, so only
    // the lookup decides, and by name it would find the first.
    applyStatusOf(pSettings.get(),
            WifiSettings::kStateConnected,
            QStringLiteral("Home"),
            QStringLiteral("bbbb"));

    EXPECT_TRUE(pSettings->isActiveNow(rowNamed(pSettings.get(), QStringLiteral("HomeOld"))));
    EXPECT_FALSE(pSettings->isActiveNow(rowNamed(pSettings.get(), QStringLiteral("Home"))));
    EXPECT_QSTRING_EQ(QStringLiteral("Connected to HomeOld"), pSettings->statusLine());
}

TEST_F(WifiSettingsStateTest, ForgetDeletesTheActiveProfileByUuidWhenTwoShareAName) {
    const auto pSettings = newSettings();
    scriptConnected(QStringLiteral("Home"), QStringLiteral("bbbb"));
    refresh(pSettings.get());
    runScan(pSettings.get(),
            QStringLiteral("Home:aaaa:802-11-wireless\n"
                           "Home:bbbb:802-11-wireless\n"),
            QStringLiteral("connection.id:Home\n"
                           "connection.uuid:aaaa\n"
                           "802-11-wireless.ssid:Home\n"
                           "\n"
                           "connection.id:Home\n"
                           "connection.uuid:bbbb\n"
                           "802-11-wireless.ssid:HomeOld\n"),
            QStringLiteral("*:HomeOld:60:WPA2\n"
                           " :Home:70:WPA2\n"));
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("HomeOld")));
    ASSERT_EQ(WifiSettings::kPageManage, pageOf(pSettings.get()));
    expectAsync(kActionArgs);
    // From here on: the scan's own SSID read names both uuids.
    const qsizetype before = asyncCalls().size();

    writeCo("forget", 1.0);

    EXPECT_QSTRING_EQ(
            argvString(QStringList{"--wait", "10", "connection", "delete", "uuid", "bbbb"}),
            lastAsync());
    // Never by the shared name, which could take both.
    EXPECT_FALSE(anyAsyncHas(QStringLiteral("id"), before));
    EXPECT_FALSE(anyAsyncHas(QStringLiteral("aaaa"), before));
}

TEST_F(WifiSettingsStateTest, ForgetRefusesWhenTwoProfilesShareTheNameAndTheUuidIsUnknown) {
    const auto pSettings = newSettings();
    scriptConnected(QStringLiteral("Home"));
    // The read that names the connection's UUID fails, every time.
    scriptSyncFailure(kIpv4Args, QStringLiteral("Error: timeout"));
    refresh(pSettings.get());
    ASSERT_EQ(WifiSettings::kStateConnected, pSettings->state());
    runScan(pSettings.get(),
            QStringLiteral("Home:aaaa:802-11-wireless\n"
                           "Home:bbbb:802-11-wireless\n"),
            QStringLiteral("connection.id:Home\n"
                           "connection.uuid:aaaa\n"
                           "802-11-wireless.ssid:Home\n"
                           "\n"
                           "connection.id:Home\n"
                           "connection.uuid:bbbb\n"
                           "802-11-wireless.ssid:HomeOld\n"),
            QStringLiteral("*:HomeOld:60:WPA2\n"
                           " :Home:70:WPA2\n"));
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("HomeOld")));
    ASSERT_EQ(WifiSettings::kPageManage, pageOf(pSettings.get()));
    expectAsync(kActionArgs);
    const qsizetype before = asyncCalls().size();

    writeCo("forget", 1.0);

    EXPECT_EQ(before, asyncCalls().size()) << lastAsync().toStdString();
    EXPECT_FALSE(anyAsyncHas(QStringLiteral("delete")));
    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_EQ(0.0, readCo("forget"));
}

TEST_F(WifiSettingsStateTest, AFailedSnapshotReadDropsAStaleSnapshot) {
    // No real path leaves a snapshot behind (every way a join ends consumes
    // it), so one is planted, to pin the defensive reset: an empty one names
    // nothing, and would let the failed join delete a profile it cannot know
    // it created.
    const auto pSettings = newSettings();
    scriptDisconnected();
    refresh(pSettings.get());
    seedRows(pSettings.get(), twoOpenNetworks());
    seedStaleSnapshot(pSettings.get(), QStringList());
    scriptSyncFailure(kSnapshotArgs, QStringLiteral("Error: NetworkManager is not running."));
    expectAsync(kActionArgs);
    pSettings->activateRow(rowIndex(pSettings.get(), QStringLiteral("CafeGuest")));
    ASSERT_EQ(WifiSettings::kPageJoining, pageOf(pSettings.get()));
    const qsizetype joinCall = asyncCalls().size() - 1;

    failOp(pSettings.get(),
            4,
            QStringLiteral("Error: Connection activation failed: (53) The Wi-Fi network "
                           "could not be found."));

    EXPECT_FALSE(anyAsyncHas(QStringLiteral("delete"), joinCall)) << lastAsync().toStdString();
    EXPECT_EQ(joinCall + 1, asyncCalls().size());
}

// --- One slow status read ------------------------------------------------------

TEST_F(WifiSettingsStateTest, ASlowStatusReadKeepsTheKeypadAndWhatWasTyped) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(0);
    type(pSettings.get(), QStringLiteral("abcde"));

    applyStatusOf(pSettings.get(), WifiSettings::kStateNotResponding);

    EXPECT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    EXPECT_EQ(5, pSettings->passwordLength());
    EXPECT_QSTRING_EQ(QStringLiteral("Alpha"), pSettings->joinTarget());

    // And nothing typed there can act until a status read succeeds: a legal
    // length is refused, and no nmcli runs.
    type(pSettings.get(), QStringLiteral("fgh"));
    pSettings->submitJoin();
    EXPECT_EQ(WifiSettings::kPagePassword, pageOf(pSettings.get()));
    EXPECT_EQ(8, pSettings->passwordLength());
    EXPECT_TRUE(syncCalls().isEmpty());
    EXPECT_TRUE(asyncCalls().isEmpty());
}

TEST_F(WifiSettingsStateTest, LosingTheRadioClosesTheKeypad) {
    const auto pSettings = newSettings();
    seedRows(pSettings.get(), twoNewSecuredNetworks());
    pSettings->activateRow(0);
    type(pSettings.get(), QStringLiteral("abcde"));

    applyStatusOf(pSettings.get(), WifiSettings::kStateRadioOff);

    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
    EXPECT_EQ(0, pSettings->passwordLength());
    EXPECT_TRUE(pSettings->joinTarget().isEmpty());
}

TEST_F(WifiSettingsStateTest, ASlowStatusReadStillClosesTheManagePage) {
    const auto pSettings = newSettings();
    applyStatusOf(pSettings.get(), WifiSettings::kStateConnected, QStringLiteral("Home"));
    QList<WifiRow> rows;
    WifiRow home;
    home.ssid = QStringLiteral("Home");
    home.signalPercent = 70;
    home.secured = true;
    home.active = true;
    rows.append(home);
    seedRowsOn(pSettings.get(), rows, QStringLiteral("Home"));
    pSettings->activateRow(0);
    ASSERT_EQ(WifiSettings::kPageManage, pageOf(pSettings.get()));

    applyStatusOf(pSettings.get(), WifiSettings::kStateNotResponding);

    EXPECT_EQ(WifiSettings::kPageList, pageOf(pSettings.get()));
}
