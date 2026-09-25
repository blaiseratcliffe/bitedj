#include "widget/wvisualssetlist.h"

#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>
#include <cmath>

#include "moc_wvisualssetlist.cpp"
#include "skin/legacy/skincontext.h"
#include "util/math.h"

namespace {
const char* kScrollAreaObjectName = "VisualsSetScrollArea";
const char* kScrollContentObjectName = "VisualsSetScrollContent";
const char* kRowObjectName = "VisualsSetRow";
const char* kActiveProperty = "active";
const char* kEmptyProperty = "empty";

// Same threshold as WWifiList and WUsbList.
constexpr int kMinDragStartDistancePx = 12;
constexpr int kScrollSingleStepPx = 24;

int dragStartDistance() {
    return math_max(QApplication::startDragDistance(), kMinDragStartDistancePx);
}

void restyle(QStyle* pStyle, QWidget* pWidget) {
    pStyle->unpolish(pWidget);
    pStyle->polish(pWidget);
}

// A QPushButton reads '&' as a mnemonic marker; set names are typed freely.
QString buttonTextFor(const QString& name) {
    QString text = name;
    text.replace(QLatin1Char('&'), QStringLiteral("&&"));
    return text;
}
} // namespace

WVisualsSetList::WVisualsSetList(QWidget* parent)
        : WWidget(parent),
          m_pScrollArea(new QScrollArea(this)),
          m_pContent(new QWidget(m_pScrollArea)),
          m_pLayout(new QGridLayout(m_pContent)),
          m_pEmptyRow(nullptr),
          m_returnPage(VisualsSets::kPageRows),
          m_dragState(DragState::Idle),
          m_lastGlobalY(0),
          m_remainingDy(0) {
    setAttribute(Qt::WA_StyledBackground, true);
    // Synthesized moves carry no button; see WWifiList.
    setMouseTracking(true);

    auto* pOuterLayout = new QVBoxLayout(this);
    pOuterLayout->setContentsMargins(0, 0, 0, 0);
    pOuterLayout->setSpacing(0);
    pOuterLayout->addWidget(m_pScrollArea);

    m_pScrollArea->setObjectName(kScrollAreaObjectName);
    m_pScrollArea->setFrameShape(QFrame::NoFrame);
    m_pScrollArea->setWidgetResizable(true);
    m_pScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pScrollArea->setFocusPolicy(Qt::NoFocus);
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

    if (VisualsSets* pSets = VisualsSets::tryInstance()) {
        connect(pSets, &VisualsSets::entriesChanged, this, &WVisualsSetList::setEntries);
        rebuildRows(pSets->entries(), pSets->activeRow());
    } else {
        renderEmpty(tr("Visuals sets unavailable"));
    }
}

WVisualsSetList::~WVisualsSetList() = default;

void WVisualsSetList::setup(const QDomNode& node, const SkinContext& context) {
    bool ok = false;
    const int page = context.selectInt(node, QStringLiteral("ReturnPage"), &ok);
    setReturnPage(ok ? page : VisualsSets::kPageRows);
}

void WVisualsSetList::setReturnPage(int page) {
    m_returnPage = page;
}

void WVisualsSetList::setEntries(const QList<VisualsSets::Entry>& entries, int activeRow) {
    rebuildRows(entries, activeRow);
}

void WVisualsSetList::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(e);
        return;
    }
    QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
    if (pScrollBar->isVisible() &&
            pScrollBar->rect().contains(
                    pScrollBar->mapFromGlobal(e->globalPosition().toPoint()))) {
        m_dragState = DragState::ScrollBar;
        m_pPressedRow.clear();
        forwardToScrollBar(e);
        e->accept();
        return;
    }
    // Hold the press until it is known to be a tap or a drag; a tap is
    // dispatched on release.
    m_dragState = DragState::Pending;
    m_pressGlobalPos = e->globalPosition();
    const int index = rowIndexAt(m_pressGlobalPos.toPoint());
    m_pPressedRow = index >= 0 ? m_rows.at(index) : nullptr;
    m_lastGlobalY = m_pressGlobalPos.y();
    m_remainingDy = 0;
    e->accept();
}

void WVisualsSetList::mouseMoveEvent(QMouseEvent* e) {
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
            e->accept();
            return;
        }
        m_dragState = DragState::Scrolling;
    }
    // Scroll bar values are integers; carry the remainder to the next move.
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

void WVisualsSetList::mouseReleaseEvent(QMouseEvent* e) {
    const DragState state = m_dragState;
    m_dragState = DragState::Idle;
    const QPointer<QPushButton> pPressedRow = m_pPressedRow;
    m_pPressedRow.clear();
    if (state == DragState::ScrollBar) {
        forwardToScrollBar(e);
        e->accept();
        return;
    }
    if (state == DragState::Pending) {
        // A tap counts only on the very row the press went down on.
        const int index = rowIndexAt(e->globalPosition().toPoint());
        if (index >= 0 && pPressedRow && m_rows.at(index) == pPressedRow.data()) {
            activateRowAt(index);
            e->accept();
            return;
        }
    }
    WWidget::mouseReleaseEvent(e);
}

int WVisualsSetList::rowIndexAt(const QPoint& globalPos) const {
    const QWidget* pViewport = m_pScrollArea->viewport();
    if (!pViewport->rect().contains(pViewport->mapFromGlobal(globalPos))) {
        return -1;
    }
    for (int i = 0; i < m_rows.size(); ++i) {
        const QPushButton* pRow = m_rows.at(i);
        if (pRow->rect().contains(pRow->mapFromGlobal(globalPos))) {
            return i;
        }
    }
    return -1;
}

void WVisualsSetList::forwardToScrollBar(QMouseEvent* pEvent) {
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

void WVisualsSetList::onRowClicked() {
    // Desktop path. A button detached by a rebuild is no longer in m_rows,
    // so indexOf gives -1 and nothing happens.
    activateRowAt(m_rows.indexOf(qobject_cast<QPushButton*>(sender())));
}

void WVisualsSetList::activateRowAt(int index) {
    if (index < 0) {
        return;
    }
    emit rowActivated(index);
    if (VisualsSets* pSets = VisualsSets::tryInstance()) {
        // Rebuilds this list through entriesChanged; nothing to paint here.
        pSets->selectRow(index, m_returnPage);
    }
}

void WVisualsSetList::rebuildRows(const QList<VisualsSets::Entry>& entries, int activeRow) {
    // See WWifiList::rebuildRows: detach now, delete later, and cancel a
    // pending tap because the row it pressed is gone. deleteLater() is not
    // optional: selectRow() emits entriesChanged synchronously, so this runs
    // inside the tap, and on the desktop path inside the clicked() slot of
    // one of the rows being removed.
    if (m_dragState == DragState::Pending) {
        m_dragState = DragState::Idle;
    }
    m_pPressedRow.clear();
    for (QPushButton* pRow : std::as_const(m_rows)) {
        pRow->setParent(nullptr);
        pRow->deleteLater();
    }
    m_rows.clear();
    if (m_pEmptyRow) {
        m_pEmptyRow->setParent(nullptr);
        m_pEmptyRow->deleteLater();
        m_pEmptyRow = nullptr;
    }
    for (int i = 0; i < entries.size(); ++i) {
        auto* pRow = new QPushButton(buttonTextFor(entries.at(i).name), m_pContent);
        pRow->setObjectName(kRowObjectName);
        pRow->setFocusPolicy(Qt::NoFocus);
        pRow->setProperty(kActiveProperty, i == activeRow);
        connect(pRow, &QPushButton::clicked, this, &WVisualsSetList::onRowClicked);
        m_pLayout->addWidget(pRow, i, 0);
        m_rows.append(pRow);
        restyle(style(), pRow);
    }
}

void WVisualsSetList::renderEmpty(const QString& message) {
    m_pEmptyRow = new QLabel(message, m_pContent);
    m_pEmptyRow->setObjectName(kRowObjectName);
    m_pEmptyRow->setProperty(kEmptyProperty, true);
    m_pLayout->addWidget(m_pEmptyRow, 0, 0);
    restyle(style(), m_pEmptyRow);
}
