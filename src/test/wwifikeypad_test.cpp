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
#include <QPointer>
#include <QPointingDevice>
#include <QPushButton>
#include <QSet>
#include <QSignalSpy>
#include <memory>
#include <utility>

#include "control/controlpushbutton.h"
#include "preferences/wifisettings.h"
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
    QPushButton* pKey = keyButtons().at(0);
    const QPoint start = centerOf(pKey);
    // Outside the keypad's own bounds entirely, not just a neighbouring key
    // -- landing on another key would duplicate
    // PressOnOneKeyReleaseOnAnotherTypesNothing instead of exercising the
    // "off any key" case.
    const QPoint outsideWidget = m_pKeypad->mapToGlobal(
            QPoint(m_pKeypad->width() + 50, m_pKeypad->height() / 2));

    press(start);
    EXPECT_TRUE(pKey->property("down").toBool());
    moveTo(start + QPoint(150, 0));
    moveTo(outsideWidget);
    EXPECT_FALSE(pKey->property("down").toBool())
            << "\"down\" must clear as soon as the finger leaves the key, "
               "not wait for release";
    release(outsideWidget);

    EXPECT_EQ(0, spy.count());
}

TEST_F(WWifiKeypadTest, LayoutSwitchChangesKeyTextNotWidgetPointers) {
    // QPointer, not a raw address comparison: a raw pointer surviving in a
    // second findChildren() call is not proof the original object is still
    // alive (a freed allocation can be reused at the same address), while
    // QPointer::isNull() reports real destruction directly.
    QList<QPointer<QPushButton>> before;
    for (QPushButton* pButton : keyButtons()) {
        before.append(QPointer<QPushButton>(pButton));
    }
    QPushButton* pLayoutSwitch = namedButton("WifiKeyLayout");
    ASSERT_NE(nullptr, pLayoutSwitch);
    EXPECT_EQ(QStringLiteral("q"), before.at(0)->text());
    EXPECT_EQ(QStringLiteral("123"), pLayoutSwitch->text());

    tap(centerOf(pLayoutSwitch));

    for (const QPointer<QPushButton>& pButton : std::as_const(before)) {
        ASSERT_FALSE(pButton.isNull())
                << "layout switch must not destroy/rebuild the key widgets";
    }
    const QList<QPushButton*> after = keyButtons();
    ASSERT_EQ(before.size(), after.size());
    for (int i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before.at(i).data(), after.at(i));
    }
    EXPECT_EQ(QStringLiteral("1"), after.at(0)->text());
    EXPECT_EQ(QStringLiteral("#+="), pLayoutSwitch->text());

    tap(centerOf(pLayoutSwitch));
    ASSERT_FALSE(before.at(0).isNull());
    EXPECT_EQ(QStringLiteral("\""), before.at(0)->text());
    EXPECT_EQ(QStringLiteral("ABC"), pLayoutSwitch->text());

    tap(centerOf(pLayoutSwitch));
    ASSERT_FALSE(before.at(0).isNull());
    EXPECT_EQ(QStringLiteral("q"), before.at(0)->text());
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

// QPushButton reads a lone '&' as a mnemonic marker and draws nothing for
// it, which left the '&' key blank. Its label is "&&" (one literal '&' on
// screen), while what it types is still the single raw character.
TEST_F(WWifiKeypadTest, AmpersandKeyShowsItsLabelAndTypesARawAmpersand) {
    QSignalSpy spy(m_pKeypad.get(), &WWifiKeypad::characterTyped);
    tap(centerOf(namedButton("WifiKeyLayout"))); // -> "123", which holds '&'

    QPushButton* pAmpersand = nullptr;
    for (QPushButton* pKey : keyButtons()) {
        if (pKey->text() == QStringLiteral("&&")) {
            pAmpersand = pKey;
        }
    }
    ASSERT_NE(nullptr, pAmpersand) << "no key labelled \"&&\" in the 123 layout";

    tap(centerOf(pAmpersand));
    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(QChar('&'), spy.at(0).at(0).toChar());
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
    // No WifiSettings exists while this test runs: WifiSettingsStateTest, in
    // the same binary, constructs one per test and destroys it before the
    // test ends, and gtest runs tests one at a time. So this also covers the
    // real stock-Mixxx fallback: tryInstance() == nullptr.
    ASSERT_EQ(nullptr, WifiSettings::tryInstance());
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

// Fix round 1: QPushButton's default vertical size policy is Fixed, so a
// key that only ever got setMinimumSize(44, 44) stays pinned near 44px tall
// even when its row's stretch factor gives the row itself much more height
// -- leaving a dead band below the key, on the real page, where a tap hits
// nothing. This resizes well above the widget's minimum and checks that two
// vertically adjacent keys are separated by exactly the grid's own spacing,
// which is only possible if both rows actually grew to fill the space they
// were given.
TEST_F(WWifiKeypadTest, KeyRowsFillTheirHeightWithNoDeadBandBetweenRows) {
    constexpr int kTallFixtureHeight = 900; // well above the ~300px minimum
    constexpr int kGridVerticalSpacing = 6; // matches wwifikeypad.cpp
    m_pKeypad->resize(kKeypadWidth, kTallFixtureHeight);
    layOut();

    QPushButton* pRow2Key = keyButtons().at(0);  // 'q', row 2, column 0
    QPushButton* pRow3Key = keyButtons().at(10); // 'a', row 3, same column

    const int gap = pRow3Key->y() - (pRow2Key->y() + pRow2Key->height());
    EXPECT_EQ(kGridVerticalSpacing, gap)
            << "a bigger gap means a key is still stuck at its "
               "minimum/Fixed height instead of filling its row";
}

TEST_F(WWifiKeypadTest, DownPropertyTracksThePressedKey) {
    QPushButton* pKey = keyButtons().at(0);

    press(centerOf(pKey));
    EXPECT_TRUE(pKey->property("down").toBool());

    release(centerOf(pKey));
    EXPECT_FALSE(pKey->property("down").toBool());
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

// Ruling 23: these tests drive the keypad with the mouse events
// WWidget::event() synthesizes, not real touches, because QTest::touchEvent
// delivers nothing to widgets under the offscreen QPA mixxx-test runs on
// (checked on the Pi 2026-09-15 with a positive control: a direct
// QMouseEvent reached the keypad and typed a character; neither the
// QWidget- nor the QWindow-overload QTest::touchEvent sequence delivered
// anything, touch or synthesized mouse, to the keypad or any key). The
// touch route itself is verified with a real finger on the panel.

} // namespace
