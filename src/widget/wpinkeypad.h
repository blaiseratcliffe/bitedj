#pragma once

#include <QList>
#include <QPoint>
#include <QPushButton>
#include <QString>

#include "widget/wwidget.h"

class ControlProxy;
class QDomNode;
class QLabel;
class SkinContext;

// Bite DJ: the four-digit keypad that changes the visuals admin PIN
// ([BiteDJ],visuals_admin_pin, which pi/bin/bitedj-visuals-admin reads out
// of mixxx.cfg). Page 2 of the Visuals page's [BiteDJ],visuals_page stack.
//
// Not WWifiKeypad: that one writes every key into WifiSettings and is a
// full keyboard. This one is a compact block: title and field share the top
// row, the digits sit in two rows of five with a Del key, and Cancel and
// Save share the bottom row, six columns in all. Keys are at least 60x60
// and a fixed 60 high. The grid (PinKeypadBody) sits between two stretches,
// no wider than 480px and no narrower than about 406px (six 60px columns,
// five 6px gaps, 8px margins), and about 258px high; those figures are
// computed, not measured, and the buttons' own size hints under the skin's
// QSS decide where in that range it lands. On the 1280px FLX6 page it
// stays centred and the keys do not stretch into slabs.
//
// Touch handling is WWifiKeypad's: every key is hit-tested on this outer
// widget on press and on release, a key commits only when both land on it,
// "down" is the touch stand-in for :pressed, and clicked() is the desktop
// path. Save is disabled below four digits and a fifth digit is ignored.
// Save writes the number (0042 is 42) to visuals_admin_pin and the return
// page (<ReturnPage>, default 0) to visuals_page; Cancel writes only the
// page. Both go through ControlProxy::set(), whose sender is the proxy and
// not the ControlObject, so SystemSettings' persistence hears the PIN.
// Hiding clears the digits. The field shows the digits typed in the clear,
// padded with middle dots: the PIN is printed on the Admin row anyway, so
// masking it here would only hide a typo.
//
// pinSaved() and cancelled() mirror the two outcomes for tests.
//
// objectNames: PinKeypadBody, PinKeypadTitle, PinKeypadField, PinKey (the
// ten digits), PinKeyBackspace, PinKeypadCancel, PinKeypadSave. Dynamic
// property "down" on whichever key is under a press.
class WPinKeypad : public WWidget {
    Q_OBJECT
  public:
    explicit WPinKeypad(QWidget* parent = nullptr);
    ~WPinKeypad() override;

    void setup(const QDomNode& node, const SkinContext& context);
    // What setup() calls with the skin's <ReturnPage>; public for tests.
    void setReturnPage(int page);
    QString digits() const {
        return m_digits;
    }

  signals:
    void pinSaved(int pin);
    void cancelled();

  protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    // Catches QEvent::TouchCancel, which WWidget::event() does not
    // translate, so a cancelled gesture leaves no key stuck "down".
    bool event(QEvent* e) override;
    void hideEvent(QHideEvent* e) override;

  private slots:
    void onKeyClicked();

  private:
    // Index into m_allButtons of the visible, enabled key under globalPos,
    // or -1.
    int keyIndexAt(const QPoint& globalPos) const;
    void setKeyDown(int index, bool down);
    // The one place a committed key is handled, from either input path.
    void dispatchButton(QPushButton* pButton);
    void typeDigit(int digit);
    void backspace();
    void save();
    void cancel();
    // Redraws the field and enables Save at exactly four digits.
    void refresh();

    QLabel* m_pTitleLabel;
    QLabel* m_pFieldLabel;
    // Indexed by the digit each one types.
    QList<QPushButton*> m_digitButtons;
    QPushButton* m_pBackspaceButton;
    QPushButton* m_pCancelButton;
    QPushButton* m_pSaveButton;
    // Every key, in a fixed order for hit-testing. Never rebuilt.
    QList<QPushButton*> m_allButtons;
    ControlProxy* m_pPinControl;
    ControlProxy* m_pPageControl;
    QString m_digits;
    int m_returnPage;
    // Set on press, consumed on release: the key a release must match.
    int m_pressedKeyIndex;
};
