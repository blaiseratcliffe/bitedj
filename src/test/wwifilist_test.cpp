// Tests for the Bite DJ Wi-Fi network list: a press and drag scrolls the
// rows so more networks than fit the panel can be reached, while a press
// that doesn't move activates the row underneath it. This deliberately
// never constructs a real WifiSettings -- its constructor schedules a real
// nmcli call (QTimer::singleShot(0, ...) firing a status refresh), which has
// no business running in this harness -- so rows are injected through the
// public setRows() slot, the same one WifiSettings::networksChanged drives
// in the real app, and tap dispatch is observed through the rowActivated()
// signal rather than through WifiSettings::activateRow(), which is exactly
// how a widget without the singleton is meant to stay testable.
//
// Everything goes through the mouse events WWidget::event() synthesizes
// from touches (delivered to the list itself, never to its children), which
// is the only way the rows are reachable on the appliance's touchscreen.
#include "widget/wwifilist.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStyleOptionSlider>
#include <algorithm>
#include <memory>

#include "control/controlpushbutton.h"
#include "preferences/wifisettings.h"
#include "test/mixxxtest.h"

namespace {

constexpr int kRowCount = 40;
constexpr int kListWidth = 400;
constexpr int kListHeight = 120;

// Well above the drag start distance.
constexpr int kDragDistance = 60;

// Mirrors the bucketing documented in wwifilist.h's header comment, so the
// test asserts against the spec rather than against a copy of the
// implementation's control flow.
QString expectedSignalGlyph(int percent) {
    int filled = 0;
    if (percent >= 88) {
        filled = 4;
    } else if (percent >= 63) {
        filled = 3;
    } else if (percent >= 38) {
        filled = 2;
    } else if (percent >= 13) {
        filled = 1;
    }
    QString glyph;
    for (int i = 0; i < 4; ++i) {
        glyph += (i < filled) ? QChar(0x25CF) : QChar(0x25CB);
    }
    return glyph;
}

QList<WifiRow> makeRows(int count) {
    QList<WifiRow> rows;
    for (int i = 0; i < count; ++i) {
        WifiRow row;
        row.ssid = QStringLiteral("NET%1").arg(i);
        row.signalPercent = 50;
        rows.append(row);
    }
    return rows;
}

class WWifiListTest : public MixxxTest {
  protected:
    void SetUp() override {
        // Every WWidget holds a proxy on this one.
        m_pTouchShift = std::make_unique<ControlPushButton>(
                ConfigKey("[Controls]", "touch_shift"));
        m_pList = std::make_unique<WWifiList>();

        m_pList->resize(kListWidth, kListHeight);
        m_pList->show();
        QCoreApplication::processEvents();

        // Rows normally arrive from WifiSettings::networksChanged. The
        // singleton is deliberately never constructed in this harness (see
        // the file header), so drive the public slot it is connected to
        // directly.
        m_pList->setRows(makeRows(kRowCount));
        layOut();
    }

    void layOut() {
        QCoreApplication::sendPostedEvents();
        m_pList->layout()->activate();
        QCoreApplication::processEvents();
    }

    // Mimics WWidget::event()'s touch translation: the event is delivered
    // to the list itself and carries no held buttons.
    void sendTouchAsMouse(QEvent::Type type, const QPoint& globalPos) {
        const QPointF pos = m_pList->mapFromGlobal(globalPos);
        QMouseEvent event(type,
                pos,
                pos,
                QPointF(globalPos),
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                QPointingDevice::primaryPointingDevice());
        QCoreApplication::sendEvent(m_pList.get(), &event);
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

    void dragBy(const QPoint& globalPos, int dy) {
        press(globalPos);
        moveTo(globalPos + QPoint(0, dy / 2));
        moveTo(globalPos + QPoint(0, dy));
        release(globalPos + QPoint(0, dy));
    }

    QScrollArea* scrollArea() const {
        return m_pList->findChild<QScrollArea*>(QStringLiteral("WifiScrollArea"));
    }

    QScrollBar* scrollBar() const {
        return scrollArea()->verticalScrollBar();
    }

    int scrollPosition() const {
        return scrollBar()->value();
    }

    // Where the scroll bar's handle currently sits, in its own coordinates.
    QRect handleRect() const {
        QScrollBar* pScrollBar = scrollBar();
        QStyleOptionSlider option;
        option.initFrom(pScrollBar);
        option.orientation = Qt::Vertical;
        option.minimum = pScrollBar->minimum();
        option.maximum = pScrollBar->maximum();
        option.sliderPosition = pScrollBar->sliderPosition();
        option.sliderValue = pScrollBar->value();
        option.singleStep = pScrollBar->singleStep();
        option.pageStep = pScrollBar->pageStep();
        option.subControls = QStyle::SC_All;
        return pScrollBar->style()->subControlRect(QStyle::CC_ScrollBar,
                &option,
                QStyle::SC_ScrollBarSlider,
                pScrollBar);
    }

    QList<QPushButton*> ssidButtons() const {
        return m_pList->findChildren<QPushButton*>(QStringLiteral("WifiSsid"));
    }

    QList<QWidget*> rowFrames() const {
        return m_pList->findChildren<QWidget*>(QStringLiteral("WifiRow"));
    }

    QList<QLabel*> signalLabels() const {
        return m_pList->findChildren<QLabel*>(QStringLiteral("WifiSignal"));
    }

    QList<QLabel*> lockLabels() const {
        return m_pList->findChildren<QLabel*>(QStringLiteral("WifiLock"));
    }

    // Centre of a row's SSID button in global coordinates, wherever the row
    // currently sits.
    QPoint ssidButtonCenter(int index) const {
        QPushButton* pButton = ssidButtons().at(index);
        return pButton->mapToGlobal(pButton->rect().center());
    }

    std::unique_ptr<ControlPushButton> m_pTouchShift;
    std::unique_ptr<WWifiList> m_pList;
};

TEST_F(WWifiListTest, MoreRowsThanFitAreScrollable) {
    // The rows must not stretch the list past the height the skin gave it;
    // the ones that don't fit are reached by scrolling instead.
    EXPECT_EQ(kListHeight, m_pList->height());
    EXPECT_GT(scrollBar()->maximum(), 0);
    EXPECT_TRUE(scrollBar()->isVisible());
}

TEST_F(WWifiListTest, FewRowsNeedNoScrollBar) {
    m_pList->setRows(makeRows(1));
    layOut();

    EXPECT_EQ(0, scrollBar()->maximum());
    EXPECT_FALSE(scrollBar()->isVisible());
}

TEST_F(WWifiListTest, DragUpScrollsDown) {
    dragBy(ssidButtonCenter(0), -kDragDistance);

    // The content sticks to the finger, so the list scrolls by the whole
    // distance including the part travelled before the gesture was known to
    // be a scroll.
    EXPECT_EQ(kDragDistance, scrollPosition());
}

TEST_F(WWifiListTest, DragDownScrollsUp) {
    scrollBar()->setValue(scrollBar()->maximum());
    const int start = scrollPosition();

    dragBy(ssidButtonCenter(kRowCount - 1), kDragDistance);

    EXPECT_EQ(start - kDragDistance, scrollPosition());
}

TEST_F(WWifiListTest, DragOverARowDoesNotActivateIt) {
    QSignalSpy spy(m_pList.get(), &WWifiList::rowActivated);

    dragBy(ssidButtonCenter(0), -kDragDistance);

    EXPECT_EQ(0, spy.count());
}

TEST_F(WWifiListTest, TapActivatesTheRow) {
    QSignalSpy spy(m_pList.get(), &WWifiList::rowActivated);

    tap(ssidButtonCenter(0));

    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(0, spy.at(0).at(0).toInt());
}

// A scan landing (networksChanged -> setRows) between press and release
// replaces the rows, and a reordered list puts a different network under the
// same point. Releasing there must not activate that network: the DJ pressed
// NET0, and an open or saved network joins on activation with no further
// confirm. The control case, the same gesture with no rebuild, is
// TapActivatesTheRow above.
TEST_F(WWifiListTest, RebuildBetweenPressAndReleaseCancelsTheTap) {
    QSignalSpy spy(m_pList.get(), &WWifiList::rowActivated);
    const QPoint pos = ssidButtonCenter(0);

    press(pos);
    QList<WifiRow> reordered = makeRows(kRowCount);
    std::reverse(reordered.begin(), reordered.end());
    m_pList->setRows(reordered);
    layOut();
    // Precondition: the point the finger is on now holds a different
    // network, and rows are still under it, so a hit-test at release time
    // would find one.
    QPushButton* pNowUnderFinger = ssidButtons().at(0);
    ASSERT_EQ(QStringLiteral("NET%1").arg(kRowCount - 1), pNowUnderFinger->text());
    ASSERT_TRUE(pNowUnderFinger->rect().contains(pNowUnderFinger->mapFromGlobal(pos)));
    release(pos);

    EXPECT_EQ(0, spy.count());

    // Only that gesture was cancelled: a fresh tap on the new list works.
    tap(pos);
    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(0, spy.at(0).at(0).toInt());
}

TEST_F(WWifiListTest, AmpersandInAnSsidIsShownNotEatenAsAMnemonic) {
    QList<WifiRow> rows = makeRows(1);
    rows[0].ssid = QStringLiteral("AT&T");
    m_pList->setRows(rows);
    layOut();

    // QPushButton drops a lone '&' as a mnemonic marker ("ATT"); "&&" is how
    // a button is told to draw one literal '&'.
    ASSERT_EQ(1, ssidButtons().size());
    EXPECT_EQ(QStringLiteral("AT&&T"), ssidButtons().at(0)->text());

    // Display only: the tap still dispatches by row, unaffected.
    QSignalSpy spy(m_pList.get(), &WWifiList::rowActivated);
    tap(ssidButtonCenter(0));
    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(0, spy.at(0).at(0).toInt());
}

TEST_F(WWifiListTest, TapActivatesTheRowScrolledUnderTheFinger) {
    scrollBar()->setValue(scrollBar()->maximum());
    layOut();
    QSignalSpy spy(m_pList.get(), &WWifiList::rowActivated);

    // Whichever row the last one is now next to: tapping its button must
    // activate that row, not the one that used to be at those coordinates.
    const int lastRow = kRowCount - 1;
    tap(ssidButtonCenter(lastRow));

    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(lastRow, spy.at(0).at(0).toInt());
}

TEST_F(WWifiListTest, TapOnRowScrolledOutOfSightActivatesNothing) {
    scrollBar()->setValue(scrollBar()->maximum());
    layOut();
    QSignalSpy spy(m_pList.get(), &WWifiList::rowActivated);

    // Row 0's *current* (post-scroll) position is off the top of the
    // viewport -- asserted below -- and a tap there must not reach it or
    // anything else, since nothing real occupies a point outside the
    // viewport's clip. Deliberately not the position row 0's button held
    // *before* scrolling: that point is still inside the still-visible
    // viewport, just over whichever different row has scrolled up into it
    // by then, so a tap there is a legitimate hit on that other row, not a
    // test of the off-viewport case at all.
    const QPoint offViewportPos = ssidButtonCenter(0);
    ASSERT_LT(offViewportPos.y(), m_pList->mapToGlobal(QPoint(0, 0)).y());
    tap(offViewportPos);

    EXPECT_EQ(0, spy.count());
}

TEST_F(WWifiListTest, DraggingTheScrollBarScrolls) {
    QScrollBar* pScrollBar = scrollBar();
    QSignalSpy spy(m_pList.get(), &WWifiList::rowActivated);
    // The scroll bar is a plain child widget, so it never sees a touch
    // either; grabbing its handle has to work through the list's own
    // handlers.
    const QPoint handlePos = pScrollBar->mapToGlobal(handleRect().center());

    press(handlePos);
    moveTo(handlePos + QPoint(0, 20));
    release(handlePos + QPoint(0, 20));

    // Dragging the bar down scrolls the list down, and much further than
    // the 20 px of travel, since the bar is a fraction of the content's
    // height.
    EXPECT_GT(scrollPosition(), 20);
    EXPECT_EQ(0, spy.count());
}

TEST_F(WWifiListTest, EmptyRowsListRendersAnInertPlaceholder) {
    m_pList->setRows(QList<WifiRow>{});
    layOut();

    ASSERT_EQ(1, rowFrames().size());
    EXPECT_TRUE(rowFrames().at(0)->property("empty").toBool());
    EXPECT_FALSE(scrollBar()->isVisible());

    // Tapping the placeholder must not crash and must not activate a row
    // that doesn't exist.
    QSignalSpy spy(m_pList.get(), &WWifiList::rowActivated);
    tap(rowFrames().at(0)->mapToGlobal(rowFrames().at(0)->rect().center()));
    EXPECT_EQ(0, spy.count());
}

TEST_F(WWifiListTest, RowsCarryActiveAndSavedAsDynamicProperties) {
    QList<WifiRow> rows = makeRows(3);
    rows[0].active = true;
    rows[1].saved = true;
    m_pList->setRows(rows);
    layOut();

    ASSERT_EQ(3, rowFrames().size());
    EXPECT_TRUE(rowFrames().at(0)->property("active").toBool());
    EXPECT_FALSE(rowFrames().at(0)->property("saved").toBool());
    EXPECT_FALSE(rowFrames().at(1)->property("active").toBool());
    EXPECT_TRUE(rowFrames().at(1)->property("saved").toBool());
    EXPECT_FALSE(rowFrames().at(2)->property("active").toBool());
    EXPECT_FALSE(rowFrames().at(2)->property("saved").toBool());
}

TEST_F(WWifiListTest, SecuredRowsShowTheLockLabelOpenRowsDoNot) {
    QList<WifiRow> rows = makeRows(2);
    rows[0].secured = true;
    rows[1].secured = false;
    m_pList->setRows(rows);
    layOut();

    ASSERT_EQ(2, lockLabels().size());
    EXPECT_TRUE(lockLabels().at(0)->isVisible());
    EXPECT_FALSE(lockLabels().at(1)->isVisible());
}

TEST_F(WWifiListTest, SignalGlyphMatchesTheDocumentedThresholds) {
    QList<WifiRow> rows = makeRows(5);
    rows[0].signalPercent = 95; // 4 filled
    rows[1].signalPercent = 70; // 3 filled
    rows[2].signalPercent = 50; // 2 filled
    rows[3].signalPercent = 20; // 1 filled
    rows[4].signalPercent = 0;  // 0 filled
    m_pList->setRows(rows);
    layOut();

    ASSERT_EQ(5, signalLabels().size());
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(expectedSignalGlyph(rows.at(i).signalPercent), signalLabels().at(i)->text())
                << "row " << i;
    }
}

TEST(WWifiListNoSingletonTest, ConstructingWithoutWifiSettingsRendersAnInertPlaceholder) {
    // No WifiSettings exists while this test runs: WifiSettingsStateTest, in
    // the same binary, constructs one per test and destroys it before the
    // test ends, and gtest runs tests one at a time. So this also covers the
    // real stock-Mixxx fallback: tryInstance() == nullptr.
    ASSERT_EQ(nullptr, WifiSettings::tryInstance());
    WWifiList list;
    list.resize(kListWidth, kListHeight);
    list.show();
    QCoreApplication::processEvents();

    QList<QWidget*> frames = list.findChildren<QWidget*>(QStringLiteral("WifiRow"));
    ASSERT_EQ(1, frames.size());
    EXPECT_TRUE(frames.at(0)->property("empty").toBool());
}

} // anonymous namespace
