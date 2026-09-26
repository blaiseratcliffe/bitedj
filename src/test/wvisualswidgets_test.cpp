// The Visuals library's panel widgets, driven the way the touchscreen drives
// them: synthesized mouse events delivered to the outer widget, never to a
// child (wwifilist_test.cpp has the same helpers and the reason). The list
// and the label run against a real VisualsSets on a temp file, which is
// cheap (no process, no network); the keypad runs with no VisualsSets at all
// and plain controls standing in for the ones it writes.
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QHostInfo>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <memory>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "preferences/visualssets.h"
#include "test/mixxxtest.h"
#include "widget/wpinkeypad.h"
#include "widget/wvisualslabel.h"
#include "widget/wvisualssetlist.h"

namespace {

const QString kGroup = QStringLiteral("[BiteDJ]");

// Mimics WWidget::event()'s touch translation for any widget.
void sendTouchAsMouse(QWidget* pTarget, QEvent::Type type, const QPoint& globalPos) {
    const QPointF pos = pTarget->mapFromGlobal(globalPos);
    QMouseEvent event(type,
            pos,
            pos,
            QPointF(globalPos),
            Qt::LeftButton,
            Qt::NoButton,
            Qt::NoModifier,
            QPointingDevice::primaryPointingDevice());
    QCoreApplication::sendEvent(pTarget, &event);
}

void tapAt(QWidget* pTarget, const QPoint& globalPos) {
    sendTouchAsMouse(pTarget, QEvent::MouseButtonPress, globalPos);
    sendTouchAsMouse(pTarget, QEvent::MouseButtonRelease, globalPos);
}

QPoint centerOf(const QWidget* pWidget) {
    return pWidget->mapToGlobal(pWidget->rect().center());
}

void layOut(QWidget* pWidget) {
    QCoreApplication::sendPostedEvents();
    if (pWidget->layout()) {
        pWidget->layout()->activate();
    }
    QCoreApplication::processEvents();
}

QByteArray setsJson(int count) {
    QByteArray json = "{\"version\": 1, \"sets\": [";
    for (int i = 1; i <= count; ++i) {
        json += QStringLiteral("%1{\"id\": %2, \"name\": \"Set %2\", \"type\": \"random\", "
                               "\"knobs\": {}}")
                        .arg(i == 1 ? QString() : QStringLiteral(","))
                        .arg(i * 10)
                        .toUtf8();
    }
    json += "]}";
    return json;
}

class VisualsSetWidgetsTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pTouchShift = std::make_unique<ControlPushButton>(
                ConfigKey("[Controls]", "touch_shift"));
        m_pPin = std::make_unique<ControlObject>(ConfigKey(kGroup, QStringLiteral("visuals_admin_pin")));
        m_pPin->set(1234);
        ASSERT_TRUE(m_dir.isValid());
        m_path = m_dir.filePath(QStringLiteral("sets.json"));
    }

    void TearDown() override {
        m_pList.reset();
        m_pLabel.reset();
        m_pSets.reset();
    }

    void writeSets(const QByteArray& bytes) {
        QSaveFile file(m_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(bytes);
        ASSERT_TRUE(file.commit());
    }

    void makeList(int rows, int width, int height) {
        writeSets(setsJson(rows));
        m_pSets = std::make_unique<VisualsSets>(config(), m_path);
        m_pList = std::make_unique<WVisualsSetList>();
        m_pList->resize(width, height);
        m_pList->show();
        layOut(m_pList.get());
    }

    QList<QPushButton*> rowButtons() const {
        return m_pList->findChildren<QPushButton*>(QStringLiteral("VisualsSetRow"));
    }

    static double value(const char* key) {
        return ControlObject::get(ConfigKey(kGroup, QString::fromLatin1(key)));
    }

    std::unique_ptr<ControlPushButton> m_pTouchShift;
    std::unique_ptr<ControlObject> m_pPin;
    QTemporaryDir m_dir;
    QString m_path;
    std::unique_ptr<VisualsSets> m_pSets;
    std::unique_ptr<WVisualsSetList> m_pList;
    std::unique_ptr<WVisualsLabel> m_pLabel;
};

TEST_F(VisualsSetWidgetsTest, ListShowsEverythingThenTheFilesSets) {
    makeList(2, 400, 300);
    const QList<QPushButton*> rows = rowButtons();
    ASSERT_EQ(3, rows.size());
    EXPECT_QSTRING_EQ(QStringLiteral("Everything"), rows.at(0)->text());
    EXPECT_QSTRING_EQ(QStringLiteral("Set 10"), rows.at(1)->text());
    EXPECT_QSTRING_EQ(QStringLiteral("Set 20"), rows.at(2)->text());
    EXPECT_TRUE(rows.at(0)->property("active").toBool());
    EXPECT_FALSE(rows.at(1)->property("active").toBool());
}

TEST_F(VisualsSetWidgetsTest, TapSelectsTheRowAndGoesToTheReturnPage) {
    makeList(2, 400, 300);
    m_pList->setReturnPage(VisualsSets::kPageLibrary);
    ControlObject::set(ConfigKey(kGroup, QStringLiteral("visuals_page")), VisualsSets::kPageSetList);
    QSignalSpy spy(m_pList.get(), &WVisualsSetList::rowActivated);

    tapAt(m_pList.get(), centerOf(rowButtons().at(2)));

    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(2, spy.at(0).at(0).toInt());
    EXPECT_EQ(20.0, value("visuals_set"));
    EXPECT_EQ(3.0, value("visuals_page"));
    // The rebuild that selection triggers marks the new row.
    layOut(m_pList.get());
    EXPECT_TRUE(rowButtons().at(2)->property("active").toBool());
}

TEST_F(VisualsSetWidgetsTest, ADragScrollsAndSelectsNothing) {
    makeList(30, 400, 150);
    QScrollBar* pBar = m_pList->findChild<QScrollArea*>(QStringLiteral("VisualsSetScrollArea"))
                               ->verticalScrollBar();
    ASSERT_GT(pBar->maximum(), 0);
    const QPoint start = centerOf(rowButtons().at(1));

    sendTouchAsMouse(m_pList.get(), QEvent::MouseButtonPress, start);
    sendTouchAsMouse(m_pList.get(), QEvent::MouseMove, start - QPoint(0, 30));
    sendTouchAsMouse(m_pList.get(), QEvent::MouseMove, start - QPoint(0, 60));
    sendTouchAsMouse(m_pList.get(), QEvent::MouseButtonRelease, start - QPoint(0, 60));

    EXPECT_GT(pBar->value(), 0);
    EXPECT_EQ(0.0, value("visuals_set"));
}

// Three guards stop this, and they overlap: rebuildRows() drops a pending
// press and clears the pressed row, and the release compares the row it
// lands on with the row the press went down on. Any one alone keeps this
// test green, so it pins the three together, not each guard. Without all
// three, the rebuilt row 1 ("Set 10" again) sits under the finger and
// would be selected.
TEST_F(VisualsSetWidgetsTest, ARebuildBetweenPressAndReleaseCancelsTheTap) {
    makeList(2, 400, 300);
    const QPoint at = centerOf(rowButtons().at(1));
    sendTouchAsMouse(m_pList.get(), QEvent::MouseButtonPress, at);
    // The service saved while the finger was down: the rows are rebuilt and
    // whatever now sits under the finger is a new button, not what was pressed.
    writeSets(setsJson(3));
    m_pSets->reload();
    // Lay the new rows out before releasing, as wwifilist_test.cpp does.
    // Until then every new button sits at its default geometry at the
    // content's top left, nothing is under the finger, and the release
    // would dispatch nothing even with every guard removed.
    layOut(m_pList.get());
    const QList<QPushButton*> rows = rowButtons();
    ASSERT_EQ(4, rows.size());
    // Precondition: a new row, "Set 10" again, is under the finger, so a
    // hit-test at release would find one.
    ASSERT_QSTRING_EQ(QStringLiteral("Set 10"), rows.at(1)->text());
    ASSERT_TRUE(rows.at(1)->rect().contains(rows.at(1)->mapFromGlobal(at)));
    sendTouchAsMouse(m_pList.get(), QEvent::MouseButtonRelease, at);
    EXPECT_EQ(0.0, value("visuals_set"));
}

TEST_F(VisualsSetWidgetsTest, LabelShowsTheActiveNameAndFollowsIt) {
    writeSets(setsJson(2));
    m_pSets = std::make_unique<VisualsSets>(config(), m_path);
    m_pLabel = std::make_unique<WVisualsLabel>();
    m_pLabel->setField(WVisualsLabel::Field::Set);
    QLabel* pText = m_pLabel->findChild<QLabel*>(QStringLiteral("VisualsLabelText"));
    ASSERT_NE(nullptr, pText);
    EXPECT_QSTRING_EQ(QStringLiteral("Everything"), pText->text());
    m_pSets->selectRow(1);
    EXPECT_QSTRING_EQ(QStringLiteral("Set 10"), pText->text());
    // Set names come from a phone; a label must never render them as markup.
    EXPECT_EQ(Qt::PlainText, pText->textFormat());
}

TEST_F(VisualsSetWidgetsTest, AdminLabelFollowsThePin) {
    m_pSets = std::make_unique<VisualsSets>(config(), m_path);
    m_pLabel = std::make_unique<WVisualsLabel>();
    m_pLabel->setField(WVisualsLabel::Field::Admin);
    QLabel* pText = m_pLabel->findChild<QLabel*>(QStringLiteral("VisualsLabelText"));
    ASSERT_NE(nullptr, pText);
    EXPECT_TRUE(pText->text().endsWith(QStringLiteral("PIN 1234")));
    // The whole line, middle included, in the contract's shape (section
    // 1.7): "bitepi.local:7380 . 192.168.4.39 . PIN 1234", each "." a
    // U+00B7 middle dot, with "no network" standing in for an address when
    // the host has none.
    const QString dot = QString(QChar(0x00B7));
    const QString ip = VisualsSets::currentIpv4();
    const QString expected = QHostInfo::localHostName() + QStringLiteral(".local:7380 ") + dot +
            QLatin1Char(' ') + (ip.isEmpty() ? QStringLiteral("no network") : ip) +
            QLatin1Char(' ') + dot + QStringLiteral(" PIN 1234");
    EXPECT_QSTRING_EQ(expected, pText->text());
    m_pPin->set(7);
    // m_pPin->set() is the owner's own set, which does not emit valueChanged
    // on m_pPin, but the proxy inside VisualsSets still hears it.
    QCoreApplication::processEvents();
    EXPECT_TRUE(pText->text().endsWith(QStringLiteral("PIN 0007")));
}

// No VisualsSets exists: the stock-Mixxx fallback.
TEST_F(VisualsSetWidgetsTest, WithoutTheSingletonBothRenderAnInertPlaceholder) {
    ASSERT_EQ(nullptr, VisualsSets::tryInstance());
    WVisualsSetList list;
    list.resize(400, 200);
    list.show();
    layOut(&list);
    const QList<QLabel*> placeholders = list.findChildren<QLabel*>(QStringLiteral("VisualsSetRow"));
    ASSERT_EQ(1, placeholders.size());
    EXPECT_TRUE(placeholders.at(0)->property("empty").toBool());

    WVisualsLabel label;
    label.setField(WVisualsLabel::Field::Set);
    EXPECT_QSTRING_EQ(QStringLiteral("Visuals sets unavailable"),
            label.findChild<QLabel*>(QStringLiteral("VisualsLabelText"))->text());
}

class WPinKeypadTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pTouchShift = std::make_unique<ControlPushButton>(
                ConfigKey("[Controls]", "touch_shift"));
        // Plain stand-ins for the controls SystemSettings and VisualsSets own.
        m_pPin = std::make_unique<ControlObject>(ConfigKey(kGroup, QStringLiteral("visuals_admin_pin")));
        m_pPin->set(1234);
        m_pPage = std::make_unique<ControlObject>(ConfigKey(kGroup, QStringLiteral("visuals_page")));
        m_pPage->set(2);
        m_pKeypad = std::make_unique<WPinKeypad>();
        m_pKeypad->resize(480, 300);
        m_pKeypad->show();
        layOut(m_pKeypad.get());
    }

    QPushButton* digit(int d) const {
        const QList<QPushButton*> keys =
                m_pKeypad->findChildren<QPushButton*>(QStringLiteral("PinKey"));
        for (QPushButton* pKey : keys) {
            if (pKey->text() == QString::number(d)) {
                return pKey;
            }
        }
        return nullptr;
    }

    QPushButton* named(const char* objectName) const {
        return m_pKeypad->findChild<QPushButton*>(QLatin1String(objectName));
    }

    void tap(QPushButton* pButton) {
        ASSERT_NE(nullptr, pButton);
        tapAt(m_pKeypad.get(), centerOf(pButton));
    }

    QString field() const {
        return m_pKeypad->findChild<QLabel*>(QStringLiteral("PinKeypadField"))->text();
    }

    std::unique_ptr<ControlPushButton> m_pTouchShift;
    std::unique_ptr<ControlObject> m_pPin;
    std::unique_ptr<ControlObject> m_pPage;
    std::unique_ptr<WPinKeypad> m_pKeypad;
};

TEST_F(WPinKeypadTest, TenDigitKeysAndEveryKeyIsAtLeastSixtyPixels) {
    EXPECT_EQ(10, m_pKeypad->findChildren<QPushButton*>(QStringLiteral("PinKey")).size());
    for (QPushButton* pKey : m_pKeypad->findChildren<QPushButton*>()) {
        EXPECT_GE(pKey->width(), 60) << qPrintable(pKey->objectName());
        EXPECT_GE(pKey->height(), 60) << qPrintable(pKey->objectName());
    }
}

TEST_F(WPinKeypadTest, SaveIsDisabledUntilFourDigits) {
    tap(digit(4));
    tap(digit(2));
    tap(digit(7));
    EXPECT_FALSE(named("PinKeypadSave")->isEnabled());
    tap(named("PinKeypadSave"));
    EXPECT_EQ(1234.0, m_pPin->get());
    tap(digit(1));
    EXPECT_TRUE(named("PinKeypadSave")->isEnabled());
    EXPECT_EQ(QStringLiteral("4271"), m_pKeypad->digits());
}

TEST_F(WPinKeypadTest, SaveWritesThePinAndGoesToTheReturnPage) {
    m_pKeypad->setReturnPage(3);
    QSignalSpy spy(m_pKeypad.get(), &WPinKeypad::pinSaved);
    for (int d : {0, 0, 4, 2}) {
        tap(digit(d));
    }
    tap(named("PinKeypadSave"));
    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(42, spy.at(0).at(0).toInt());
    EXPECT_EQ(42.0, m_pPin->get());
    EXPECT_EQ(3.0, m_pPage->get());
    EXPECT_TRUE(m_pKeypad->digits().isEmpty());
}

TEST_F(WPinKeypadTest, AFifthDigitIsIgnoredAndBackspaceRemovesOne) {
    for (int d : {1, 2, 3, 4, 5}) {
        tap(digit(d));
    }
    EXPECT_EQ(QStringLiteral("1234"), m_pKeypad->digits());
    tap(named("PinKeyBackspace"));
    EXPECT_EQ(QStringLiteral("123"), m_pKeypad->digits());
    EXPECT_TRUE(field().startsWith(QStringLiteral("123")));
}

TEST_F(WPinKeypadTest, CancelLeavesThePinAndGoesToTheReturnPage) {
    QSignalSpy spy(m_pKeypad.get(), &WPinKeypad::cancelled);
    tap(digit(9));
    tap(named("PinKeypadCancel"));
    EXPECT_EQ(1, spy.count());
    EXPECT_EQ(1234.0, m_pPin->get());
    EXPECT_EQ(0.0, m_pPage->get());
    EXPECT_TRUE(m_pKeypad->digits().isEmpty());
}

TEST_F(WPinKeypadTest, PressOnOneKeyAndReleaseOnAnotherTypesNothing) {
    sendTouchAsMouse(m_pKeypad.get(), QEvent::MouseButtonPress, centerOf(digit(1)));
    sendTouchAsMouse(m_pKeypad.get(), QEvent::MouseButtonRelease, centerOf(digit(2)));
    EXPECT_TRUE(m_pKeypad->digits().isEmpty());
}

TEST_F(WPinKeypadTest, HidingClearsTheDigits) {
    tap(digit(5));
    m_pKeypad->hide();
    EXPECT_TRUE(m_pKeypad->digits().isEmpty());
}

} // namespace
