#pragma once

#include <QList>
#include <QPointF>
#include <QPointer>
#include <QPushButton>

#include "preferences/visualssets.h"
#include "widget/wwidget.h"

class QDomNode;
class QGridLayout;
class QLabel;
class QScrollArea;
class SkinContext;

// Bite DJ: the Visuals page's set picker, one row per VisualsSets::entries()
// with Everything first, the active one marked. A tap calls
// VisualsSets::selectRow(index, returnPage), which makes that set active and
// moves [BiteDJ],visuals_page back to the page the list was opened from
// (<ReturnPage> in the skin, default 0).
//
// Shape and gesture handling are WWifiList's (src/widget/wwifilist.h), minus
// the status line: a QScrollArea/QGridLayout content area, the
// press/move/release DragState classifier and forwardToScrollBar, because
// on this touchscreen WWidget::event() delivers every touch to this widget
// and never to a row or the scroll bar. A row is a single QPushButton
// (objectName VisualsSetRow) carrying the dynamic property "active"; its
// clicked() is the desktop path only.
//
// selectRow() emits VisualsSets::entriesChanged synchronously, so the rows
// are rebuilt from inside a tap on one of them (and, on the desktop path,
// from inside that row's own clicked() slot). Old rows are therefore
// detached and deleteLater()ed, never deleted, as in WWifiList.
//
// A rebuild while a finger is down cancels the tap, as in WWifiList: the
// service can save the file at any moment, and the row under the finger
// afterwards may be a different set. Dispatch also emits rowActivated(),
// unconditionally, so a test can see where a gesture landed.
//
// Set names come from a file written over the LAN. A QPushButton never
// renders markup, but it does read '&' as a mnemonic marker, so each name
// has its '&' doubled for display.
//
// Without the singleton: one placeholder QLabel, objectName VisualsSetRow,
// property "empty", reading "Visuals sets unavailable".
//
// objectNames a skin can style: VisualsSetScrollArea,
// VisualsSetScrollContent, VisualsSetRow.
class WVisualsSetList : public WWidget {
    Q_OBJECT
  public:
    explicit WVisualsSetList(QWidget* parent = nullptr);
    ~WVisualsSetList() override;

    void setup(const QDomNode& node, const SkinContext& context);
    // What setup() calls with the skin's <ReturnPage>; public for tests.
    void setReturnPage(int page);

  public slots:
    // Connected to VisualsSets::entriesChanged.
    void setEntries(const QList<VisualsSets::Entry>& entries, int activeRow);

  signals:
    void rowActivated(int index);

  protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

  private slots:
    void onRowClicked();

  private:
    enum class DragState {
        // No press to act on.
        Idle,
        // Press seen, not yet classified as a tap or a drag.
        Pending,
        // The content is following the finger.
        Scrolling,
        // The press landed on the scroll bar, which is driving the scroll.
        ScrollBar,
    };

    void rebuildRows(const QList<VisualsSets::Entry>& entries, int activeRow);
    void renderEmpty(const QString& message);
    // Emits rowActivated(index) and, if the singleton exists, calls
    // VisualsSets::selectRow(index, m_returnPage).
    void activateRowAt(int index);
    // Index of the row under globalPos, or -1. Only a point inside the
    // visible viewport can hit a row.
    int rowIndexAt(const QPoint& globalPos) const;
    void forwardToScrollBar(QMouseEvent* pEvent);

    QScrollArea* m_pScrollArea;
    QWidget* m_pContent;
    QGridLayout* m_pLayout;
    QList<QPushButton*> m_rows;
    QLabel* m_pEmptyRow;
    int m_returnPage;

    DragState m_dragState;
    QPointF m_pressGlobalPos;
    // The row the press went down on; a tap acts on this row only. A
    // QPointer because rebuildRows() deleteLater()s the old rows.
    QPointer<QPushButton> m_pPressedRow;
    qreal m_lastGlobalY;
    // Sub-pixel remainder of the drag not yet applied to the scroll bar.
    qreal m_remainingDy;
};
