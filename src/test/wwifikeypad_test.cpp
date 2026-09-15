// Tests for the Bite DJ Wi-Fi password keypad. This deliberately never
// constructs a real WifiSettings -- its constructor schedules a real nmcli
// call, which has no business running in this harness -- so every action is
// observed through the widget's own characterTyped/backspaceTyped/
// cancelRequested/joinRequested signals, exactly the way a keypad without
// the singleton is meant to stay testable (see wwifikeypad.h).
//
// Everything goes through the mouse events WWidget::event() synthesizes
// from touches (delivered to the keypad itself, never to a key), which is
// the only way the keys are reachable on the appliance's touchscreen --
// mirrors wwifilist_test.cpp's press/move/release helpers.
#include "widget/wwifikeypad.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <memory>
#include <utility>

#include "control/controlpushbutton.h"
#include "test/mixxxtest.h"

namespace {

constexpr int kKeypadWidth = 1280;
constexpr int kKeypadHeight = 420;

class WWifiKeypadTest : public MixxxTest {
  protected:
    void SetUp() override {
        // Every WWidget holds a proxy on this one.
        m_pTouchShift = std::make_unique<ControlPushButton>(
                ConfigKey("[Controls]", "touch_shift"));
        m_pKeypad = std::make_unique<WWifiKeypad>();

        m_pKeypad->resize(kKeypadWidth, kKeypadHeight);
        m_pKeypad->show();
        layOut();
    }

    void layOut() {
        QCoreApplication::sendPostedEvents();
        m_pKeypad->layout()->activate();
        QCoreApplication::processEvents();
    }

    // Mimics WWidget::event()'s touch translation: the event is delivered to
    // the keypad itself and carries no held buttons.
    void sendTouchAsMouse(QEvent::Type type, const QPoint& globalPos) {
        const QPointF pos = m_pKeypad->mapFromGlobal(globalPos);
        QMouseEvent event(type,
                pos,
                pos,
                QPointF(globalPos),
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                QPointingDevice::primaryPointingDevice());
        QCoreApplication::sendEvent(m_pKeypad.get(), &event);
    }

    void press(const QPoint& globalPos) {
        sendTouchAsMouse(QEvent::MouseButtonPress, globalPos);
    }

    void moveTo(const QPoint& globalPos) {
        sendTouchAsMouse(QEvent::MouseMove, globalPos);
    }

    void release(const QPoint& globalPos) {
        sendTouchAsMouse(QEvent::MouseButtonRelease, globalPos);
    }

    void tap(const QPoint& globalPos) {
        press(globalPos);
        release(globalPos);
    }

    QPoint centerOf(const QPushButton* pButton) const {
        return pButton->mapToGlobal(pButton->rect().center());
    }

    // The 26-slot character grid, in the fixed construction order
    // documented in wwifikeypad.h: Q-row, A-row, Z-row.
    QList<QPushButton*> keyButtons() const {
        return m_pKeypad->findChildren<QPushButton*>(QStringLiteral("WifiKey"));
    }

    QPushButton* namedButton(const char* objectName) const {
        return m_pKeypad->findChild<QPushButton*>(QLatin1String(objectName));
    }

    std::unique_ptr<ControlPushButton> m_pTouchShift;
    std::unique_ptr<WWifiKeypad> m_pKeypad;
};

TEST_F(WWifiKeypadTest, TapTypesExactlyOneCharacter) {
    QSignalSpy spy(m_pKeypad.get(), &WWifiKeypad::characterTyped);

    // keyButtons().at(0) is the first key built, 'q' in the default layout.
    tap(centerOf(keyButtons().at(0)));

    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(QChar('q'), spy.at(0).at(0).toChar());
}

TEST_F(WWifiKeypadTest, PressOnOneKeyReleaseOnAnotherTypesNothing) {
    QSignalSpy spy(m_pKeypad.get(), &WWifiKeypad::characterTyped);
    const QList<QPushButton*> keys = keyButtons();

    press(centerOf(keys.at(0)));
    release(centerOf(keys.at(1)));

    EXPECT_EQ(0, spy.count());
}

TEST_F(WWifiKeypadTest, PressThenDragOffThenReleaseTypesNothing) {
    QSignalSpy spy(m_pKeypad.get(), &WWifiKeypad::characterTyped);
    const QPoint start = centerOf(keyButtons().at(0));
    // Comfortably more than one key's width away (~120 px at this size), so
    // the release lands off the pressed key, whether on a neighbour or on
    // the gap the A-row leaves in its last column.
    const QPoint draggedOff = start + QPoint(300, 0);

    press(start);
    moveTo(start + QPoint(150, 0));
    moveTo(draggedOff);
    release(draggedOff);

    EXPECT_EQ(0, spy.count());
}

TEST_F(WWifiKeypadTest, LayoutSwitchChangesKeyTextNotWidgetPointers) {
    const QList<QPushButton*> before = keyButtons();
    QPushButton* pLayoutSwitch = namedButton("WifiKeyLayout");
    ASSERT_NE(nullptr, pLayoutSwitch);
    const QString firstKeyTextBefore = before.at(0)->text();
    EXPECT_EQ(QStringLiteral("q"), firstKeyTextBefore);
    EXPECT_EQ(QStringLiteral("123"), pLayoutSwitch->text());

    tap(centerOf(pLayoutSwitch));

    const QList<QPushButton*> after = keyButtons();
    EXPECT_EQ(before, after) << "layout switch must not destroy/rebuild the key widgets";
    EXPECT_EQ(QStringLiteral("1"), after.at(0)->text());
    EXPECT_EQ(QStringLiteral("#+="), pLayoutSwitch->text());

    tap(centerOf(pLayoutSwitch));
    EXPECT_EQ(QStringLiteral("\""), keyButtons().at(0)->text());
    EXPECT_EQ(QStringLiteral("ABC"), pLayoutSwitch->text());

    tap(centerOf(pLayoutSwitch));
    EXPECT_EQ(QStringLiteral("q"), keyButtons().at(0)->text());
    EXPECT_EQ(QStringLiteral("123"), pLayoutSwitch->text());
}

TEST_F(WWifiKeypadTest, ShiftUppercasesOnlyTheNextLetterThenResets) {
    QSignalSpy spy(m_pKeypad.get(), &WWifiKeypad::characterTyped);
    QPushButton* pShift = namedButton("WifiKeyShift");
    ASSERT_NE(nullptr, pShift);
    QPushButton* pQ = keyButtons().at(0);

    tap(centerOf(pShift));
    EXPECT_TRUE(pShift->property("shiftActive").toBool());
    EXPECT_EQ(QStringLiteral("Q"), pQ->text());

    tap(centerOf(pQ));
    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(QChar('Q'), spy.at(0).at(0).toChar());
    EXPECT_FALSE(pShift->property("shiftActive").toBool()) << "Shift is one-shot";
    EXPECT_EQ(QStringLiteral("q"), pQ->text());

    tap(centerOf(pQ));
    ASSERT_EQ(2, spy.count());
    EXPECT_EQ(QChar('q'), spy.at(1).at(0).toChar());
}

TEST_F(WWifiKeypadTest, ShiftDoesNothingOutsideTheLettersLayout) {
    QPushButton* pShift = namedButton("WifiKeyShift");
    QPushButton* pLayoutSwitch = namedButton("WifiKeyLayout");
    tap(centerOf(pLayoutSwitch)); // -> "123"

    EXPECT_FALSE(pShift->isEnabled());
    tap(centerOf(pShift));
    EXPECT_FALSE(pShift->property("shiftActive").toBool());
}

TEST_F(WWifiKeypadTest, SpaceTypesASpaceCharacter) {
    QSignalSpy spy(m_pKeypad.get(), &WWifiKeypad::characterTyped);
    tap(centerOf(namedButton("WifiKeySpace")));

    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(QChar(' '), spy.at(0).at(0).toChar());
}

TEST_F(WWifiKeypadTest, BackspaceEmitsBackspaceTyped) {
    QSignalSpy spy(m_pKeypad.get(), &WWifiKeypad::backspaceTyped);
    tap(centerOf(namedButton("WifiKeyBackspace")));

    EXPECT_EQ(1, spy.count());
}

TEST_F(WWifiKeypadTest, CancelAndJoinEmitTheirSignalsWithoutTheSingleton) {
    QSignalSpy cancelSpy(m_pKeypad.get(), &WWifiKeypad::cancelRequested);
    QSignalSpy joinSpy(m_pKeypad.get(), &WWifiKeypad::joinRequested);

    tap(centerOf(namedButton("WifiKeypadCancel")));
    tap(centerOf(namedButton("WifiKeypadJoin")));

    EXPECT_EQ(1, cancelSpy.count());
    EXPECT_EQ(1, joinSpy.count());
}

TEST_F(WWifiKeypadTest, InertWithoutTheSingletonStillRendersAndDoesNotCrash) {
    // WifiSettings is never constructed in this test binary, so this also
    // covers the real stock-Mixxx fallback: tryInstance() == nullptr.
    QLabel* pTitle = m_pKeypad->findChild<QLabel*>(QStringLiteral("WifiKeypadTitle"));
    QLabel* pField = m_pKeypad->findChild<QLabel*>(QStringLiteral("WifiKeypadField"));
    ASSERT_NE(nullptr, pTitle);
    ASSERT_NE(nullptr, pField);
    EXPECT_FALSE(pTitle->text().isEmpty());
    EXPECT_TRUE(pField->text().isEmpty());
}

TEST_F(WWifiKeypadTest, EveryKeyMeasuresAtLeast44Pixels) {
    QList<QPushButton*> all = keyButtons();
    for (const char* name : {"WifiKeyShift",
                 "WifiKeyLayout",
                 "WifiKeyBackspace",
                 "WifiKeySpace",
                 "WifiKeypadCancel",
                 "WifiKeypadJoin"}) {
        all.append(namedButton(name));
    }
    for (const QPushButton* pButton : std::as_const(all)) {
        EXPECT_GE(pButton->width(), 44) << pButton->objectName().toStdString();
        EXPECT_GE(pButton->height(), 44) << pButton->objectName().toStdString();
    }
}

// Ruling 18: a WPA passphrase may contain any printable ASCII character, so
// every one of 0x20-0x7E must be reachable from this keypad. This walks the
// real tap-dispatch path (not just the layout tables) across every layout,
// plus Shift, and checks the union against the full range -- exactly, no
// more and no less.
TEST_F(WWifiKeypadTest, AllPrintableAsciiIsReachableAcrossLayoutsAndShift) {
    QSignalSpy spy(m_pKeypad.get(), &WWifiKeypad::characterTyped);
    QSet<QChar> collected;
    auto collectSpy = [&]() {
        for (const QList<QVariant>& args : std::as_const(spy)) {
            collected.insert(args.at(0).toChar());
        }
        spy.clear();
    };

    QPushButton* pShift = namedButton("WifiKeyShift");
    QPushButton* pLayoutSwitch = namedButton("WifiKeyLayout");
    QPushButton* pSpace = namedButton("WifiKeySpace");
    const QList<QPushButton*> keys = keyButtons();

    // Layout 0 ("abc"), lowercase: every slot is used.
    for (QPushButton* pKey : keys) {
        tap(centerOf(pKey));
    }
    collectSpy();

    // Layout 0, uppercase. Shift is one-shot, so it is re-armed before every
    // key -- this also exercises the reset behaviour 26 times over, not
    // just once.
    for (QPushButton* pKey : keys) {
        tap(centerOf(pShift));
        tap(centerOf(pKey));
    }
    collectSpy();

    tap(centerOf(pSpace));
    collectSpy();

    // Layout 1 ("123"): some slots are blank/disabled by design.
    tap(centerOf(pLayoutSwitch));
    for (QPushButton* pKey : keys) {
        if (pKey->isEnabled()) {
            tap(centerOf(pKey));
        }
    }
    collectSpy();

    // Layout 2 ("symbols"): likewise, more slots blank than used.
    tap(centerOf(pLayoutSwitch));
    for (QPushButton* pKey : keys) {
        if (pKey->isEnabled()) {
            tap(centerOf(pKey));
        }
    }
    collectSpy();

    QSet<QChar> expected;
    for (int c = 0x20; c <= 0x7E; ++c) {
        expected.insert(QChar(c));
    }
    EXPECT_EQ(expected, collected);
}

} // namespace
