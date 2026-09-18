#include "widget/wwifilist.h"

#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>
#include <cmath>

#include "moc_wwifilist.cpp"
#include "preferences/wifisettings.h"
#include "skin/legacy/skincontext.h"
#include "util/math.h"

namespace {
const char* kStatusLabelObjectName = "WifiStatusLine";
const char* kScrollAreaObjectName = "WifiScrollArea";
const char* kScrollContentObjectName = "WifiScrollContent";
const char* kRowObjectName = "WifiRow";
const char* kSsidObjectName = "WifiSsid";
const char* kSignalObjectName = "WifiSignal";
const char* kLockObjectName = "WifiLock";
const char* kActiveProperty = "active";
const char* kSavedProperty = "saved";
const char* kEmptyProperty = "empty";

// Distance the finger has to travel vertically before the gesture counts as
// a scroll instead of a tap. Same threshold as WUsbList and
// TouchScrollFilter: Qt's own drag distance is meant for a mouse and is
// easily exceeded by the jitter of a fingertip.
constexpr int kMinDragStartDistancePx = 12;
// Roughly a third of a row per wheel notch / scroll bar step.
constexpr int kScrollSingleStepPx = 24;

int dragStartDistance() {
    return math_max(QApplication::startDragDistance(), kMinDragStartDistancePx);
}

// Repolishes a widget so a property change picked up by the stylesheet
// ([active], [saved], [empty]) is actually repainted.
void restyle(QStyle* pStyle, QWidget* pWidget) {
    pStyle->unpolish(pWidget);
    pStyle->polish(pWidget);
}

// Four glyphs, U+25CF (filled) and U+25CB (empty), bucketed off the row's
// 0..100 signal percentage. See the header comment for the thresholds.
QString signalGlyph(int percent) {
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

// QPushButton reads '&' in its text as a mnemonic marker: it drops the '&'
// and underlines the next character, so an SSID "AT&T" would show as "ATT".
// SSIDs are whatever the broadcaster chose, so double every '&' to show it
// literally. Display only: nothing reads the text back, dispatch goes by row
// index.
QString buttonTextFor(const QString& ssid) {
    QString text = ssid;
    text.replace(QLatin1Char('&'), QStringLiteral("&&"));
    return text;
}
} // namespace

WWifiList::WWifiList(QWidget* parent)
        : WWidget(parent),
          m_pStatusLabel(new QLabel(this)),
          m_pScrollArea(new QScrollArea(this)),
          m_pContent(new QWidget(m_pScrollArea)),
          m_pLayout(new QGridLayout(m_pContent)),
          m_pEmptyRow(nullptr),
          m_visibleToBackend(false),
          m_dragState(DragState::Idle),
          m_lastGlobalY(0),
          m_remainingDy(0) {
    setAttribute(Qt::WA_StyledBackground, true);
    // The mouse events WWidget synthesizes from touches carry no held
    // button, and Qt drops a button-less move unless the widget tracks the
    // mouse -- so without this a finger drag would never be seen as one.
    setMouseTracking(true);

    auto* pOuterLayout = new QVBoxLayout(this);
    pOuterLayout->setContentsMargins(0, 0, 0, 0);
    pOuterLayout->setSpacing(0);

    m_pStatusLabel->setObjectName(kStatusLabelObjectName);
    m_pStatusLabel->setWordWrap(true);
    // Ruling 22: this label's text can include the connected SSID
    // (untrusted, broadcast by anyone nearby); AutoText would let one
    // containing markup render as rich text. The per-row SSID text (below,
    // in rebuildRows()) is a QPushButton's text, not a QLabel's -- a button
    // never renders markup, so it has no rich-text risk. It is not quite
    // plain text either: a button reads '&' as a mnemonic marker and drops
    // it, which rebuildRows() undoes with buttonTextFor().
    m_pStatusLabel->setTextFormat(Qt::PlainText);
    pOuterLayout->addWidget(m_pStatusLabel);
    pOuterLayout->addWidget(m_pScrollArea);

    m_pScrollArea->setObjectName(kScrollAreaObjectName);
    m_pScrollArea->setFrameShape(QFrame::NoFrame);
    m_pScrollArea->setWidgetResizable(true);
    m_pScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pScrollArea->setFocusPolicy(Qt::NoFocus);
    // The skin paints the panel behind us; the viewport must not cover it.
    m_pScrollArea->viewport()->setAutoFillBackground(false);
    m_pScrollArea->verticalScrollBar()->setSingleStep(kScrollSingleStepPx);

    m_pContent->setObjectName(kScrollContentObjectName);
    m_pContent->setAutoFillBackground(false);
    m_pScrollArea->setWidget(m_pContent);

    m_pLayout->setContentsMargins(0, 0, 0, 0);
    m_pLayout->setHorizontalSpacing(0);
    m_pLayout->setVerticalSpacing(6);
    m_pLayout->setColumnStretch(0, 1);
    // Keep rows pinned to the top instead of spreading down the panel.
    m_pLayout->setRowStretch(1000, 1);

    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        connect(pSettings, &WifiSettings::networksChanged, this, &WWifiList::setRows);
        connect(pSettings, &WifiSettings::statusChanged, this, &WWifiList::onStatusChanged);
        onStatusChanged(pSettings->statusLine(), pSettings->state());
        rebuildRows(pSettings->rows());
    } else {
        renderEmpty(tr("Wi-Fi settings unavailable"));
    }
}

WWifiList::~WWifiList() {
    setVisibleToBackend(false);
}

void WWifiList::setup(const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration; everything is driven by WifiSettings.
}

void WWifiList::showEvent(QShowEvent* e) {
    WWidget::showEvent(e);
    setVisibleToBackend(true);
}

void WWifiList::hideEvent(QHideEvent* e) {
    WWidget::hideEvent(e);
    setVisibleToBackend(false);
}

void WWifiList::setVisibleToBackend(bool visible) {
    if (m_visibleToBackend == visible) {
        return;
    }
    m_visibleToBackend = visible;
    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        pSettings->setClientVisible(this, visible);
    }
}

void WWifiList::setRows(const QList<WifiRow>& rows) {
    rebuildRows(rows);
}

void WWifiList::onStatusChanged(const QString& statusLine, int /*state*/) {
    m_pStatusLabel->setText(statusLine);
}

void WWifiList::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(e);
        return;
    }

    // Map via global coords so the hit-test is correct regardless of which
    // widget the synthesized event's local position referenced, and
    // regardless of how far the content is scrolled.
    QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
    if (pScrollBar->isVisible() &&
            pScrollBar->rect().contains(
                    pScrollBar->mapFromGlobal(e->globalPosition().toPoint()))) {
        m_dragState = DragState::ScrollBar;
        m_pPressedFrame.clear();
        forwardToScrollBar(e);
        e->accept();
        return;
    }

    // Hold the press back until we know whether this is a tap or a drag;
    // the row activation is dispatched on release.
    m_dragState = DragState::Pending;
    m_pressGlobalPos = e->globalPosition();
    const int pressedIndex = rowIndexAt(m_pressGlobalPos.toPoint());
    m_pPressedFrame = pressedIndex >= 0 ? m_rowFrames.at(pressedIndex) : nullptr;
    m_lastGlobalY = m_pressGlobalPos.y();
    m_remainingDy = 0;
    e->accept();
}

void WWifiList::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragState == DragState::Idle) {
        WWidget::mouseMoveEvent(e);
        return;
    }
    if (m_dragState == DragState::ScrollBar) {
        forwardToScrollBar(e);
        e->accept();
        return;
    }

    const qreal globalY = e->globalPosition().y();
    if (m_dragState == DragState::Pending) {
        if (std::abs(globalY - m_pressGlobalPos.y()) < dragStartDistance()) {
            // Might still become a tap, keep swallowing.
            e->accept();
            return;
        }
        m_dragState = DragState::Scrolling;
        // m_lastGlobalY is still the press position, so the content catches
        // up with the finger in this first step and stays pinned to it.
    }

    // Scroll bar values are integers, carry the remainder over to the next
    // move so slow drags don't get lost in rounding.
    m_remainingDy += m_lastGlobalY - globalY;
    const int scrollBy = static_cast<int>(m_remainingDy);
    if (scrollBy != 0) {
        m_remainingDy -= scrollBy;
        QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
        pScrollBar->setValue(pScrollBar->value() + scrollBy);
    }
    m_lastGlobalY = globalY;
    e->accept();
}

void WWifiList::mouseReleaseEvent(QMouseEvent* e) {
    const DragState state = m_dragState;
    m_dragState = DragState::Idle;
    const QPointer<QWidget> pPressedFrame = m_pPressedFrame;
    m_pPressedFrame.clear();

    if (state == DragState::ScrollBar) {
        forwardToScrollBar(e);
        e->accept();
        return;
    }
    if (state == DragState::Pending) {
        // The finger never moved: a tap after all. It counts only if the
        // row under the finger now is the very frame it went down on. A
        // rebuild in between already cancelled the gesture (see
        // rebuildRows), so this is belt and braces: comparing frames rather
        // than indices means a new list can never pass for the old one, even
        // if the same index now holds a different network.
        const QPoint globalPos = e->globalPosition().toPoint();
        const int index = rowIndexAt(globalPos);
        if (index >= 0 && pPressedFrame && m_rowFrames.at(index) == pPressedFrame.data()) {
            activateRowAt(index);
            e->accept();
            return;
        }
    }
    WWidget::mouseReleaseEvent(e);
}

int WWifiList::rowIndexAt(const QPoint& globalPos) const {
    // Rows scrolled out of sight still have a geometry, but it is clipped
    // away by the viewport, so only taps landing inside the viewport can
    // hit one.
    const QWidget* pViewport = m_pScrollArea->viewport();
    if (!pViewport->rect().contains(pViewport->mapFromGlobal(globalPos))) {
        return -1;
    }
    for (int i = 0; i < m_rowFrames.size(); ++i) {
        const QWidget* pFrame = m_rowFrames.at(i);
        if (pFrame->rect().contains(pFrame->mapFromGlobal(globalPos))) {
            return i;
        }
    }
    return -1;
}

void WWifiList::forwardToScrollBar(QMouseEvent* pEvent) {
    QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
    const QPointF localPos = pScrollBar->mapFromGlobal(pEvent->globalPosition().toPoint());
    const bool isRelease = pEvent->type() == QEvent::MouseButtonRelease;
    const bool isMove = pEvent->type() == QEvent::MouseMove;
    const QPointingDevice* pDevice = pEvent->pointingDevice()
            ? pEvent->pointingDevice()
            : QPointingDevice::primaryPointingDevice();
    QMouseEvent forwarded(pEvent->type(),
            localPos,
            localPos,
            pEvent->globalPosition(),
            isMove ? Qt::NoButton : Qt::LeftButton,
            isRelease ? Qt::NoButton : Qt::LeftButton,
            pEvent->modifiers(),
            pDevice);
    QCoreApplication::sendEvent(pScrollBar, &forwarded);
}

void WWifiList::onRowClicked() {
    // Desktop/mouse path: a real QMouseEvent reaches the child button
    // directly and emits clicked(). (On the touchscreen this never fires;
    // the press and release handlers above do the dispatching instead.)
    // A mouse press lands on the button itself, so this path is already
    // tied to the pressed row: if a rebuild replaced the rows mid-click, the
    // sender is an old, detached button no longer in m_ssidButtons, indexOf
    // gives -1 and activateRowAt ignores it.
    const int index = m_ssidButtons.indexOf(qobject_cast<QPushButton*>(sender()));
    activateRowAt(index);
}

void WWifiList::activateRowAt(int index) {
    if (index < 0) {
        return;
    }
    emit rowActivated(index);
    if (WifiSettings* pSettings = WifiSettings::tryInstance()) {
        pSettings->activateRow(index);
    }
}

void WWifiList::rebuildRows(const QList<WifiRow>& rows) {
    // Detach old row widgets immediately (so they stop painting) but
    // deleteLater so Qt can finish dispatching the click event that
    // triggered the rebuild -- see WUsbList::rebuildRows for why a
    // synchronous delete from inside a button's own clicked handler crashes.
    //
    // A rebuild mid-gesture cancels a pending tap: the row the finger went
    // down on is gone, and whatever now sits under it is a network the DJ
    // never chose. They tap again on the new list. A drag already under way
    // (Scrolling, ScrollBar) carries on, since it only moves the scroll bar
    // and activates nothing.
    if (m_dragState == DragState::Pending) {
        m_dragState = DragState::Idle;
    }
    m_pPressedFrame.clear();
    for (QWidget* pFrame : std::as_const(m_rowFrames)) {
        pFrame->setParent(nullptr);
        pFrame->deleteLater();
    }
    m_rowFrames.clear();
    m_ssidButtons.clear();
    if (m_pEmptyRow) {
        m_pEmptyRow->setParent(nullptr);
        m_pEmptyRow->deleteLater();
        m_pEmptyRow = nullptr;
    }

    if (rows.isEmpty()) {
        renderEmpty(tr("No networks found"));
        return;
    }

    // The backend decides what a tap on a row does; it decides what the row
    // looks like too. Without an instance (the unit test harness) the scanned
    // flag is all there is, and it is what the backend would answer anyway.
    WifiSettings* pSettings = WifiSettings::tryInstance();

    for (int i = 0; i < rows.size(); ++i) {
        const WifiRow& row = rows.at(i);

        auto* pFrame = new QWidget(m_pContent);
        pFrame->setObjectName(kRowObjectName);
        auto* pRowLayout = new QHBoxLayout(pFrame);
        pRowLayout->setContentsMargins(0, 0, 0, 0);
        pRowLayout->setSpacing(6);

        auto* pSsid = new QPushButton(buttonTextFor(row.ssid), pFrame);
        pSsid->setObjectName(kSsidObjectName);
        pSsid->setFocusPolicy(Qt::NoFocus);
        connect(pSsid, &QPushButton::clicked, this, &WWifiList::onRowClicked);
        pRowLayout->addWidget(pSsid, 1);

        auto* pSignal = new QLabel(signalGlyph(row.signalPercent), pFrame);
        pSignal->setObjectName(kSignalObjectName);
        pRowLayout->addWidget(pSignal, 0);

        auto* pLock = new QLabel(tr("Secured"), pFrame);
        pLock->setObjectName(kLockObjectName);
        pLock->setVisible(row.secured);
        pRowLayout->addWidget(pLock, 0);

        // Not row.active, which is only what the last scan saw: after a link
        // change the backend already distrusts it (isActiveNow()), so a row
        // styled from the raw flag would be highlighted as the current
        // network while a tap on it started a join, until the rescan landed.
        pFrame->setProperty(kActiveProperty, pSettings ? pSettings->isActiveNow(row) : row.active);
        pFrame->setProperty(kSavedProperty, row.saved);

        m_pLayout->addWidget(pFrame, i, 0);
        m_rowFrames.append(pFrame);
        m_ssidButtons.append(pSsid);

        restyle(style(), pFrame);
        restyle(style(), pSsid);
        restyle(style(), pSignal);
        restyle(style(), pLock);
    }
}

void WWifiList::renderEmpty(const QString& message) {
    m_pEmptyRow = new QLabel(message, m_pContent);
    m_pEmptyRow->setObjectName(kRowObjectName);
    m_pEmptyRow->setProperty(kEmptyProperty, true);
    m_pLayout->addWidget(m_pEmptyRow, 0, 0);
    restyle(style(), m_pEmptyRow);
}
