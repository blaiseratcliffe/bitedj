// VisualsSets against a real file in a temp directory: the sets file the
// admin service writes (~/.bitedj-visuals-sets.json on the Pi), read the way
// the app reads it. What this is here to catch: a knob applied at startup
// over the panel's own later changes, a knob that changes on screen but never
// reaches mixxx.cfg's persistence path, the revision counter bumping on a
// save that changed nothing (the page would reload for nothing) or starting
// where a previous run left off, an active set that was deleted staying
// active, a broken file resetting the active set, a re-pick of the active set
// that does nothing, a sub-page that survives leaving the Settings page, and
// a watcher that goes blind after the first atomic rename.
#include "preferences/visualssets.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDateTime>
#include <QHostInfo>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>
#include <utility>
#include <vector>

#include "control/controlobject.h"
#include "test/mixxxtest.h"

namespace {

const QString kGroup = QStringLiteral("[BiteDJ]");

// Two stored sets. Id 5 comes first on purpose: entries() must follow the
// file's order, not sort by id.
const QByteArray kTwoSets = QByteArrayLiteral(R"({
  "version": 1, "nextId": 6,
  "known": {"sketches": [], "patterns": [], "clips": []},
  "sets": [
    {"id": 5, "name": "Random DnB", "type": "random",
     "sketches": ["tunnel"], "patterns": [], "clips": [],
     "knobs": {"bars": 32, "reactivity": 2, "bounce": 0, "swirl": 3, "camMix": 0}},
    {"id": 2, "name": "Opener", "type": "sequence",
     "entries": [{"sketch": "tunnel"}],
     "knobs": {"bars": 16, "reactivity": 1, "bounce": 2, "swirl": 2, "camMix": 1}}
  ]
})");

// kTwoSets with only set 2 renamed: an edit that does not touch set 5.
const QByteArray kSet2Renamed = QByteArray(kTwoSets).replace("\"Opener\"", "\"Closer\"");
// kTwoSets with set 5's bars changed from 32 to 8.
const QByteArray kSet5Bars8 = QByteArray(kTwoSets).replace("\"bars\": 32", "\"bars\": 8");
// kTwoSets without set 2.
const QByteArray kOnlySet5 = QByteArrayLiteral(R"({
  "version": 1, "nextId": 6,
  "sets": [
    {"id": 5, "name": "Random DnB", "type": "random",
     "sketches": ["tunnel"], "patterns": [], "clips": [],
     "knobs": {"bars": 32, "reactivity": 2, "bounce": 0, "swirl": 3, "camMix": 0}}
  ]
})");

class VisualsSetsTest : public MixxxTest {
  protected:
    void SetUp() override {
        ASSERT_TRUE(m_dir.isValid());
        m_path = m_dir.filePath(QStringLiteral("sets.json"));
        // The knobs and the PIN, as SystemSettings creates them, with its
        // defaults. VisualsSets creates visuals_set, visuals_set_rev and
        // visuals_page itself.
        const std::pair<const char*, double> knobs[] = {
                {"visuals_bars", 16.0},
                {"visuals_reactivity", 1.0},
                {"visuals_bounce", 2.0},
                {"visuals_swirl", 2.0},
                {"visuals_cam_mix", 1.0},
                {"visuals_admin_pin", 1234.0},
        };
        for (const auto& [key, value] : knobs) {
            const ConfigKey configKey(kGroup, QString::fromLatin1(key));
            auto pCo = std::make_unique<ControlObject>(configKey);
            pCo->set(value);
            // The persistence SystemSettings attaches to each knob, in the
            // same shape (systemsettings.cpp:259-265). A knob written with a
            // member set() on the object getControl() returns never reaches
            // this lambda (controlobject.cpp:49-54), so a test that reads the
            // config back catches that bug.
            UserSettingsPointer pConfig = config();
            QObject::connect(pCo.get(),
                    &ControlObject::valueChanged,
                    pCo.get(),
                    [pConfig, configKey](double v) {
                        pConfig->setValue(configKey, v);
                    });
            m_controls.push_back(std::move(pCo));
        }
    }

    void TearDown() override {
        m_pSets.reset();
    }

    // The Settings stack's page control, which the skin parser creates in
    // the app. Tests that need it call this before or after makeSets().
    void makeSettingsTab() {
        m_pSettingsTab = std::make_unique<ControlObject>(
                ConfigKey(QStringLiteral("[SettingsTab]"), QStringLiteral("current")));
        m_pSettingsTab->set(7);
    }

    int configInt(const char* key) const {
        return config()->getValue(ConfigKey(kGroup, QString::fromLatin1(key)), -1);
    }

    // Written the way the service writes it: a temp file renamed over the
    // real name, so the watcher sees a directory change and a new inode.
    void writeSets(const QByteArray& bytes) {
        QSaveFile file(m_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(bytes.size(), file.write(bytes));
        ASSERT_TRUE(file.commit());
    }

    void makeSets() {
        m_pSets = std::make_unique<VisualsSets>(config(), m_path);
    }

    static double value(const char* key) {
        return ControlObject::get(ConfigKey(kGroup, QString::fromLatin1(key)));
    }

    static void setValue(const char* key, double v) {
        ControlObject::set(ConfigKey(kGroup, QString::fromLatin1(key)), v);
    }

    QTemporaryDir m_dir;
    QString m_path;
    std::vector<std::unique_ptr<ControlObject>> m_controls;
    std::unique_ptr<ControlObject> m_pSettingsTab;
    std::unique_ptr<VisualsSets> m_pSets;
};

TEST_F(VisualsSetsTest, MissingFileListsOnlyEverything) {
    const double before = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    makeSets();
    const double after = static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const QList<VisualsSets::Entry> entries = m_pSets->entries();
    ASSERT_EQ(1, entries.size());
    EXPECT_EQ(0, entries.at(0).id);
    EXPECT_QSTRING_EQ(QStringLiteral("Everything"), entries.at(0).name);
    EXPECT_EQ(0, m_pSets->activeRow());
    EXPECT_EQ(0.0, value("visuals_set"));
    // Seeded with the start time and not bumped by the first read: a bump
    // would put it one past a second read after construction.
    EXPECT_GE(value("visuals_set_rev"), before);
    EXPECT_LE(value("visuals_set_rev"), after);
    EXPECT_EQ(0.0, value("visuals_page"));
}

// Chromium outlives an app restart, and the page rereads the file only when
// (set, setRev) differs from the last pair it saw. Two starts must therefore
// never produce the same rev. A real restart takes far longer than the
// second waited for here.
TEST_F(VisualsSetsTest, EachStartSeedsANewRevision) {
    makeSets();
    const double first = value("visuals_set_rev");
    EXPECT_GT(first, 0.0);
    m_pSets.reset();
    QTest::qWait(1100);
    makeSets();
    EXPECT_GT(value("visuals_set_rev"), first);
}

TEST_F(VisualsSetsTest, EntriesFollowTheFileOrder) {
    writeSets(kTwoSets);
    makeSets();
    const QList<VisualsSets::Entry> entries = m_pSets->entries();
    ASSERT_EQ(3, entries.size());
    EXPECT_EQ(0, entries.at(0).id);
    EXPECT_EQ(5, entries.at(1).id);
    EXPECT_QSTRING_EQ(QStringLiteral("Random DnB"), entries.at(1).name);
    EXPECT_EQ(2, entries.at(2).id);
    EXPECT_QSTRING_EQ(QStringLiteral("Opener"), entries.at(2).name);
}

TEST_F(VisualsSetsTest, UnreadableFileListsOnlyEverythingAndKeepsTheActiveId) {
    config()->setValue(ConfigKey(kGroup, QStringLiteral("visuals_set")), 2);
    writeSets(QByteArrayLiteral("{ not json"));
    makeSets();
    EXPECT_EQ(1, m_pSets->entries().size());
    EXPECT_EQ(2.0, value("visuals_set"));
    EXPECT_TRUE(m_pSets->activeName().contains(QStringLiteral("unreadable")));

    // Fixing the file brings the active set straight back.
    writeSets(kTwoSets);
    m_pSets->reload();
    EXPECT_EQ(2.0, value("visuals_set"));
    EXPECT_EQ(2, m_pSets->activeRow());
    EXPECT_QSTRING_EQ(QStringLiteral("Opener"), m_pSets->activeName());
}

TEST_F(VisualsSetsTest, WrongVersionIsUnreadable) {
    config()->setValue(ConfigKey(kGroup, QStringLiteral("visuals_set")), 5);
    writeSets(QByteArray(kTwoSets).replace("\"version\": 1", "\"version\": 2"));
    makeSets();
    EXPECT_EQ(1, m_pSets->entries().size());
    EXPECT_EQ(5.0, value("visuals_set"));
}

TEST_F(VisualsSetsTest, NoKnobsAreAppliedAtConstruction) {
    config()->setValue(ConfigKey(kGroup, QStringLiteral("visuals_set")), 5);
    writeSets(kTwoSets);
    makeSets();
    EXPECT_EQ(5.0, value("visuals_set"));
    EXPECT_EQ(16.0, value("visuals_bars"));
    EXPECT_EQ(1.0, value("visuals_reactivity"));
    EXPECT_EQ(-1, configInt("visuals_bars"));
}

TEST_F(VisualsSetsTest, SelectingASetAppliesItsKnobsAndPersistsThem) {
    writeSets(kTwoSets);
    makeSets();
    setValue("visuals_set", 5);
    EXPECT_EQ(32.0, value("visuals_bars"));
    EXPECT_EQ(2.0, value("visuals_reactivity"));
    EXPECT_EQ(0.0, value("visuals_bounce"));
    EXPECT_EQ(3.0, value("visuals_swirl"));
    EXPECT_EQ(0.0, value("visuals_cam_mix"));
    // Each knob reached its persistence lambda, not only the control.
    EXPECT_EQ(32, configInt("visuals_bars"));
    EXPECT_EQ(2, configInt("visuals_reactivity"));
    EXPECT_EQ(0, configInt("visuals_bounce"));
    EXPECT_EQ(3, configInt("visuals_swirl"));
    EXPECT_EQ(0, configInt("visuals_cam_mix"));
    EXPECT_EQ(5, configInt("visuals_set"));
    EXPECT_QSTRING_EQ(QStringLiteral("Random DnB"), m_pSets->activeName());
}

// visuals_set ignores a write of the value it already holds
// (control.cpp:294), so picking the active set again fires nothing unless
// selectRow applies the knobs itself. That re-pick is how the panel gets a
// set's knobs back after changing one.
TEST_F(VisualsSetsTest, PickingTheActiveSetAgainReappliesItsKnobs) {
    writeSets(kTwoSets);
    makeSets();
    m_pSets->selectRow(1);
    ASSERT_EQ(5.0, value("visuals_set"));
    setValue("visuals_bars", 64);
    setValue("visuals_page", VisualsSets::kPageSetList);
    m_pSets->selectRow(1);
    EXPECT_EQ(32.0, value("visuals_bars"));
    EXPECT_EQ(32, configInt("visuals_bars"));
    EXPECT_EQ(0.0, value("visuals_page"));
}

TEST_F(VisualsSetsTest, EverythingLeavesTheKnobsAlone) {
    writeSets(kTwoSets);
    makeSets();
    setValue("visuals_set", 5);
    setValue("visuals_bars", 64);
    setValue("visuals_set", 0);
    EXPECT_EQ(64.0, value("visuals_bars"));
    EXPECT_QSTRING_EQ(QStringLiteral("Everything"), m_pSets->activeName());
}

TEST_F(VisualsSetsTest, SelectRowSetsTheIdAndTheReturnPage) {
    writeSets(kTwoSets);
    makeSets();
    setValue("visuals_page", VisualsSets::kPageSetList);
    m_pSets->selectRow(2);
    EXPECT_EQ(2.0, value("visuals_set"));
    EXPECT_EQ(0.0, value("visuals_page"));

    setValue("visuals_page", VisualsSets::kPageSetList);
    m_pSets->selectRow(1, VisualsSets::kPageLibrary);
    EXPECT_EQ(5.0, value("visuals_set"));
    EXPECT_EQ(3.0, value("visuals_page"));

    // Out of range: nothing moves.
    m_pSets->selectRow(9, 0);
    EXPECT_EQ(5.0, value("visuals_set"));
    EXPECT_EQ(3.0, value("visuals_page"));
}

TEST_F(VisualsSetsTest, RevisionBumpsOnlyWhenTheBytesChange) {
    writeSets(kTwoSets);
    makeSets();
    const double seed = value("visuals_set_rev");
    m_pSets->reload();
    EXPECT_EQ(seed, value("visuals_set_rev"));
    // The service rewrites the file on every save; an identical save is not
    // an edit and the page must not reload for it.
    writeSets(kTwoSets);
    m_pSets->reload();
    EXPECT_EQ(seed, value("visuals_set_rev"));
    writeSets(kSet2Renamed);
    m_pSets->reload();
    EXPECT_EQ(seed + 1.0, value("visuals_set_rev"));
}

// Leaving the page for another settings sub-tab and coming back must show
// the rows, not the list or keypad left open. The tab is changed with the
// static set, which is the sender-less write a sub-tab button makes.
TEST_F(VisualsSetsTest, ChangingTheSettingsTabResetsTheSubPage) {
    makeSettingsTab();
    makeSets();
    setValue("visuals_page", VisualsSets::kPageLibrary);
    ControlObject::set(ConfigKey(QStringLiteral("[SettingsTab]"), QStringLiteral("current")), 3);
    EXPECT_EQ(0.0, value("visuals_page"));
}

// In the app the skin parser creates [SettingsTab],current after this class
// exists, so the proxy has to be bound late: opening a sub-page binds it.
TEST_F(VisualsSetsTest, ASettingsTabCreatedLaterIsStillWatched) {
    makeSets();
    makeSettingsTab();
    setValue("visuals_page", VisualsSets::kPagePinKeypad);
    ControlObject::set(ConfigKey(QStringLiteral("[SettingsTab]"), QStringLiteral("current")), 3);
    EXPECT_EQ(0.0, value("visuals_page"));
}

TEST_F(VisualsSetsTest, DeletedActiveSetFallsBackToEverything) {
    writeSets(kTwoSets);
    makeSets();
    setValue("visuals_set", 2);
    writeSets(kOnlySet5);
    m_pSets->reload();
    EXPECT_EQ(0.0, value("visuals_set"));
    EXPECT_EQ(0, config()->getValue(ConfigKey(kGroup, QStringLiteral("visuals_set")), -1));
    EXPECT_EQ(0, m_pSets->activeRow());
}

TEST_F(VisualsSetsTest, EditedKnobsOfTheActiveSetAreApplied) {
    writeSets(kTwoSets);
    makeSets();
    setValue("visuals_set", 5);
    ASSERT_EQ(32.0, value("visuals_bars"));
    writeSets(kSet5Bars8);
    m_pSets->reload();
    EXPECT_EQ(8.0, value("visuals_bars"));
}

TEST_F(VisualsSetsTest, AnUnrelatedEditKeepsThePanelsKnobChanges) {
    writeSets(kTwoSets);
    makeSets();
    setValue("visuals_set", 5);
    setValue("visuals_bars", 64);
    writeSets(kSet2Renamed);
    m_pSets->reload();
    EXPECT_EQ(64.0, value("visuals_bars"));
}

TEST_F(VisualsSetsTest, AnOutOfRangeKnobIsSkippedAndTheRestApply) {
    writeSets(QByteArray(kTwoSets).replace("\"bars\": 32", "\"bars\": 12"));
    makeSets();
    setValue("visuals_set", 5);
    EXPECT_EQ(16.0, value("visuals_bars"));
    EXPECT_EQ(3.0, value("visuals_swirl"));
    EXPECT_EQ(0.0, value("visuals_bounce"));
}

TEST_F(VisualsSetsTest, TheWatcherSeesAnAtomicRename) {
    writeSets(kTwoSets);
    makeSets();
    QSignalSpy spy(m_pSets.get(), &VisualsSets::entriesChanged);
    const double seed = value("visuals_set_rev");
    // Twice, because the first rename replaces the inode a file watch was on.
    writeSets(kSet2Renamed);
    ASSERT_TRUE(QTest::qWaitFor([seed] { return value("visuals_set_rev") >= seed + 1.0; }, 5000));
    writeSets(kOnlySet5);
    ASSERT_TRUE(QTest::qWaitFor([seed] { return value("visuals_set_rev") >= seed + 2.0; }, 5000));
    EXPECT_EQ(2, m_pSets->entries().size());
    EXPECT_GE(spy.count(), 2);
}

TEST_F(VisualsSetsTest, AdminInfoZeroPadsThePinAndFollowsIt) {
    makeSets();
    const QString host = QHostInfo::localHostName() + QStringLiteral(".local:7380");
    EXPECT_TRUE(m_pSets->adminInfo().startsWith(host)) << qPrintable(m_pSets->adminInfo());
    EXPECT_TRUE(m_pSets->adminInfo().endsWith(QStringLiteral("PIN 1234")));

    QSignalSpy spy(m_pSets.get(), &VisualsSets::adminInfoChanged);
    setValue("visuals_admin_pin", 42);
    EXPECT_TRUE(m_pSets->adminInfo().endsWith(QStringLiteral("PIN 0042")));
    EXPECT_EQ(1, spy.count());

    // Anything that is not a whole number from 0 to 9999 reads as the default.
    setValue("visuals_admin_pin", 12345);
    EXPECT_TRUE(m_pSets->adminInfo().endsWith(QStringLiteral("PIN 1234")));
    EXPECT_QSTRING_EQ(QStringLiteral("0000"), VisualsSets::formatPin(0));
    EXPECT_QSTRING_EQ(QStringLiteral("1234"), VisualsSets::formatPin(12.5));
    EXPECT_QSTRING_EQ(QStringLiteral("1234"), VisualsSets::formatPin(-1));
}

TEST_F(VisualsSetsTest, TheSingletonIsClearedOnDestruction) {
    makeSets();
    EXPECT_EQ(m_pSets.get(), VisualsSets::tryInstance());
    m_pSets.reset();
    EXPECT_EQ(nullptr, VisualsSets::tryInstance());
}

} // namespace
