#pragma once

#include <QString>

#include "widget/wwidget.h"

class ControlProxy;
class QDomNode;
class QLabel;
class SkinContext;

// Bite DJ: the Network settings page's status readout, meant to sit above
// WWifiList and (Phase 4's) WWifiKeypad on every one of the Network page's
// four WidgetStack pages (settings.xml, Phase 3). Two QLabels: a title
// (objectName WifiStatusTitle) and a detail line (objectName
// WifiStatusDetail), stacked top to bottom in a QVBoxLayout with a stretch
// after them, so on a page sized much taller than the two labels (page 3
// fills the whole Network page) they stay pinned together at the top rather
// than each claiming a share of the leftover height -- a QLabel's default
// size policy is Preferred, not Fixed, so without that trailing stretch a
// plain QVBoxLayout spreads them apart to fill whatever space it's given.
// Fonts, colors and horizontal padding are left to the skin's QSS, same as
// every other widget in this file.
//
// This widget reads [Wifi],page through a ControlProxy rather than
// listening for a WifiSettings signal. The contract's signal list
// (networksChanged / statusChanged / joinTargetChanged / passwordChanged)
// has none for the page itself, and adding one just for this one reader
// would be a new piece of the shared Phase 1/2/4 contract for a single
// consumer; the control socket already carries the value (the same one
// `co.sh get '[Wifi],page'` reads), so a ControlProxy is the same read any
// other page-aware widget in this app would use.
//
// Rendering by page (WifiSettings::kPageJoining / kPageManage; every other
// page, including kPageList and kPagePassword, falls through to the
// default):
//   - page 2 (joining): title is "Joining <ssid>...", detail is blank.
//   - page 3 (manage): title is the ssid alone, detail is the status line
//     (e.g. "Connected to <ssid> (dot) <ipv4>").
//   - default: title is the status line, detail is blank. This is what
//     page 0 (the list) wants, and is a reasonable placeholder for page 1
//     (password entry), which Phase 4's WWifiKeypad speaks for on its own.
//
// tryInstance() == nullptr leaves both labels blank rather than crashing,
// the same soft contract WUsbList and WWifiList use.
//
// objectNames a skin can style: WifiStatusTitle, WifiStatusDetail.
class WWifiStatus : public WWidget {
    Q_OBJECT
  public:
    explicit WWifiStatus(QWidget* parent = nullptr);
    ~WWifiStatus() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

  private slots:
    void onStatusChanged(const QString& statusLine, int state);
    void onJoinTargetChanged(const QString& ssid);
    void onPageChanged(double page);

  private:
    void refresh();
    // Registers or unregisters this widget with WifiSettings::
    // setClientVisible, no-op when tryInstance() is null. Idempotent, so the
    // destructor can call it unconditionally with false and only reach the
    // backend if the widget was still visible.
    void setVisibleToBackend(bool visible);

    QLabel* m_pTitleLabel;
    QLabel* m_pDetailLabel;
    // Owns [Wifi],page. Never null: a ControlProxy is safe to hold even
    // before WifiSettings creates the control it names (get() reads 0,
    // i.e. kPageList, until then).
    ControlProxy* m_pPageControl;

    QString m_statusLine;
    QString m_joinTarget;
    int m_page;
    bool m_visibleToBackend;
};
