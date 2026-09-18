#pragma once

#include <QList>
#include <QPointF>
#include <QPointer>
#include <QPushButton>
#include <QString>

#include "preferences/wifisettings.h"
#include "widget/wwidget.h"

class QDomNode;
class QGridLayout;
class QLabel;
class QScrollArea;
class SkinContext;

// Bite DJ: renders the Wi-Fi networks WifiSettings scans as a vertical stack
// of rows, the recovery path for a box that has lost its network and has no
// keyboard or ssh session to fix it with. Shape copied from WUsbList
// (src/widget/wusblist.h) almost verbatim: a QScrollArea/QGridLayout content
// area, the same press/move/release DragState classifier and
// forwardToScrollBar, because on this touchscreen WWidget::event() (see
// wwidget.cpp) synthesizes touches into mouse events delivered to THIS
// widget, never to a child row or the scroll bar. WUsbList's header has the
// full account of why; it is not repeated here.
//
// A row is SSID text / a signal-strength glyph / a lock indicator, laid out
// left to right inside one QWidget per network (objectName WifiRow), so the
// whole row -- not just the SSID text -- is one tap target. There are no
// per-row action buttons: destructive actions live on page 3, reached by
// tapping the row for the network the box is already on, which is itself a
// confirm step (no double arm/confirm needed, unlike WUsbList's Eject). A
// tap anywhere in a row calls WifiSettings::activateRow(index), which owns
// the whole dispatch (enterprise notify and stay on page 0, active row to
// page 3, open or already-saved straight to joining on page 2, a new
// secured network to the password page) -- this widget never decides a
// page itself. Every dispatch also emits rowActivated(), unconditionally,
// so a unit test built without the WifiSettings singleton (its constructor
// schedules a real nmcli call, which a test must not trigger) can still
// observe which row a gesture landed on.
//
// A non-scrolling status QLabel sits above the scroll area (objectName
// WifiStatusLine) fed by WifiSettings::statusChanged, and it never scrolls
// out of view: it is the one line a DJ in a recovery scenario needs to be
// able to read no matter how the list below it has been scrolled.
//
// Signal strength is rendered as 0-4 filled U+25CF (filled circle) out of 4,
// the rest U+25CB (empty circle), on objectName WifiSignal. DejaVu Sans,
// never Inter and never an emoji glyph: this panel doesn't render Inter
// (style.qss:1775 documents the fallback), and a plain BMP glyph is the one
// thing guaranteed to be in DejaVu Sans. The font itself is never set here;
// the skin's QSS does that. Bucketed off WifiRow::signalPercent: 4 filled at
// 88%+, 3 at 63-87%, 2 at 38-62%, 1 at 13-37%, 0 below 13%.
//
// res/skins/BiteDJ-FLX6/icons/keylock.svg was checked and does not draw a
// padlock: its path data is a two-note musical figure (it is used elsewhere
// in this skin for a track's musical key, not a security key), so it is not
// reused here. The lock indicator is instead a plain QLabel (objectName
// WifiLock) reading "Secured", shown only when the row is secured, which
// the skin is free to restyle or replace with a real icon later.
//
// A row's active and saved flags are carried as dynamic properties (active,
// saved; both bool) on its WifiRow container, restyled with
// style()->unpolish/polish whenever the rows are rebuilt -- the same idiom
// WUsbList uses for its armed/recording properties -- so the skin can give
// the connected and remembered networks a distinct look. WifiRow is also
// the objectName of the single placeholder label shown in the inert empty
// state (tryInstance() == nullptr, or a scan that found nothing); that
// placeholder carries no active/saved properties, only "empty" (bool).
//
// objectNames a skin can style: WifiStatusLine, WifiScrollArea,
// WifiScrollContent, WifiRow, WifiSsid, WifiSignal, WifiLock.
class WWifiList : public WWidget {
    Q_OBJECT
  public:
    explicit WWifiList(QWidget* parent = nullptr);
    ~WWifiList() override;

    void setup(const QDomNode& node, const SkinContext& context);

  public slots:
    // Rebuilds the rows. Connected to WifiSettings::networksChanged; also
    // how a unit test without the singleton drives this widget, since
    // constructing a real WifiSettings schedules an nmcli call.
    void setRows(const QList<WifiRow>& rows);

  signals:
    // Emitted whenever a tap lands on a row and dispatches to
    // WifiSettings::activateRow(index), whether or not the singleton
    // exists, so a test built without WifiSettings can still assert what a
    // gesture did.
    void rowActivated(int index);

  protected:
    // See WUsbList's header for why the whole gesture has to be handled
    // here rather than by the row widgets themselves.
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

  private slots:
    void onStatusChanged(const QString& statusLine, int state);
    void onRowClicked();

  private:
    enum class DragState {
        // No press to act on.
        Idle,
        // Press seen, gesture not classified yet: still either a tap or the
        // beginning of a content drag.
        Pending,
        // The content is following the finger.
        Scrolling,
        // The press landed on the scroll bar, which is driving the scroll.
        ScrollBar,
    };

    void rebuildRows(const QList<WifiRow>& rows);
    void renderEmpty(const QString& message);
    // Emits rowActivated(index) and, if the singleton exists, calls
    // WifiSettings::activateRow(index).
    void activateRowAt(int index);
    // Index of the row frame under globalPos, or -1 for none. Mirrors
    // WUsbList::buttonIndexAt, including the viewport-clipping check: a row
    // scrolled out of sight still has a geometry, just one the viewport
    // clips away, so only a tap actually inside the visible area can hit it.
    int rowIndexAt(const QPoint& globalPos) const;
    // Replays a press/move/release onto the vertical scroll bar, exactly as
    // WUsbList::forwardToScrollBar does and for the same reason (buttons are
    // forced to the left button because the synthesized events carry none).
    void forwardToScrollBar(QMouseEvent* pEvent);
    // Registers or unregisters this widget with WifiSettings::
    // setClientVisible, no-op when tryInstance() is null. Idempotent: a
    // second call with the same value does nothing, so the destructor can
    // call it unconditionally with false and only reach the backend if the
    // widget was still visible.
    void setVisibleToBackend(bool visible);

    // Rows live inside the scroll area's content widget, not directly in
    // this widget, so the list can grow past the height the skin gives us.
    QLabel* m_pStatusLabel;
    QScrollArea* m_pScrollArea;
    QWidget* m_pContent;
    QGridLayout* m_pLayout;
    // One WifiRow container per network row, kept for teardown on rebuild.
    QList<QWidget*> m_rowFrames;
    // SSID buttons, indexed parallel to m_rowFrames: the desktop/mouse
    // clicked() path, and how onRowClicked finds which row fired it.
    QList<QPushButton*> m_ssidButtons;
    QLabel* m_pEmptyRow;
    bool m_visibleToBackend;

    DragState m_dragState;
    // Press and last-seen positions in global coordinates, which stay valid
    // while the content underneath us scrolls away.
    QPointF m_pressGlobalPos;
    // The row frame the press landed on, or null if it landed on none. A tap
    // acts on this row and only this row: rows are rebuilt whenever a scan
    // lands (networksChanged), which can happen between press and release
    // and put a different network under the same point, so the release
    // hit-test alone would activate a row the DJ never pressed -- and an open
    // or saved network joins straight away. A QPointer because rebuildRows
    // deleteLater()s the old frames; it clears itself once one is gone, and
    // rebuildRows clears it immediately anyway (see there).
    QPointer<QWidget> m_pPressedFrame;
    qreal m_lastGlobalY;
    // Sub-pixel remainder of the movement not yet applied to the scroll bar.
    qreal m_remainingDy;
};
