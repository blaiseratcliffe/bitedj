#pragma once

#include <QChar>
#include <QList>
#include <QPoint>
#include <QPushButton>
#include <QString>

#include "widget/wwidget.h"

class QDomNode;
class QGridLayout;
class QLabel;
class SkinContext;

// Bite DJ: the in-skin password keypad for [Wifi],page == kPagePassword, the
// only way to type a WPA passphrase on a box with a touchscreen and no
// keyboard. `pi/bin/bitedj-run` sets QT_IM_MODULE=none (a squeekboard
// kiosk-bricking bug forced that -- see docs/M4-SKIN-NOTES.md section 12), so
// this never touches Qt's input-method framework: every key is a QPushButton
// this class owns and dispatches itself.
//
// Touch quirk (see WWifiList / WUsbList headers for the full account): on
// this touchscreen WWidget::event() synthesizes touches into mouse events
// delivered to the OUTER widget, never to a child. That includes Cancel and
// Join, not just the character keys -- a naive implementation that wired
// Cancel/Join through QPushButton::clicked() alone would work at a desk and
// be untappable by a finger on the panel. So every button on this widget,
// control keys included, is hit-tested from mousePressEvent/
// mouseReleaseEvent overridden here on the outer widget, through one shared
// dispatch (dispatchButton()) also reachable from clicked() for the desktop
// path. The two paths never both fire for one physical action: a real mouse
// click is delivered by Qt straight to the child button under the cursor
// (clicked() fires, this widget's own mousePressEvent never runs), while a
// synthesized touch is delivered to this widget and never reaches the child
// at all -- the same non-overlap WWifiList's SSID buttons and WUsbList's
// Eject/Record buttons rely on.
//
// Keypress commit happens on RELEASE, and only if release lands on the same
// key press did: mousePressEvent hit-tests and remembers the key index,
// mouseReleaseEvent hit-tests again and only dispatches when the two match.
// A finger that slides off before lifting (or lifts over a different key)
// types nothing. There is no scrolling or drag classification here (unlike
// WWifiList/WUsbList) -- this widget doesn't scroll -- so mouseMoveEvent only
// swallows the event.
//
// Key widgets are built once and never destroyed. A layout switch (Shift,
// 123, #+=) changes each key's text/enabled state in place
// (refreshLayout()), the same discipline WUsbList::rebuildRows's own header
// comment argues for: rebuilding widgets mid-gesture, e.g. while a finger is
// still down between press and release, would leave the release hit-test
// pointing at a freed button.
//
// Three layouts, reached by cycling ONE switch key (objectName
// WifiKeyLayout) rather than two separate "123" and "back" keys: its label
// always names the layout a tap will switch TO ("123" while on letters,
// "#+=" while on 123, "ABC" while on symbols), cycling 0 -> 1 -> 2 -> 0.
// Ruling 18 requires all 95 printable ASCII characters (0x20-0x7E) be
// reachable; the split below was chosen so the three layouts' emitted
// characters are pairwise disjoint and their union is exactly that range
// (verified by AllPrintableAsciiIsReachableAcrossLayoutsAndShift in
// wwifikeypad_test.cpp, which walks every layout/shift combination rather
// than trusting this comment):
//   - Layout 0, "abc" (26 keys, all 26 slots used): lowercase letters,
//     QWERTY row order (qwertyuiop / asdfghjkl / zxcvbnm). Shift
//     (objectName WifiKeyShift) uppercases them. Shift is ONE-SHOT, not
//     sticky: it applies to the next character typed and then clears itself.
//     Chosen over sticky because the field below is masked (bullets only --
//     the plaintext never leaves WifiSettings, so there is no way to notice
//     a passphrase silently typed in all caps until the join fails).
//     Switching away from "abc" always clears a pending shift, and so does
//     hiding the widget (see hideEvent()). Space is a typed character and
//     consumes a pending shift like any letter (commitTypedChar() is the
//     one place both go through). Backspace does NOT consume it: it erases
//     a character rather than typing one, and a DJ correcting a mistake
//     mid-word has no reason to lose the capital they just armed for the
//     next key.
//   - Layout 1, "123" (24 of 26 slots used, 2 blank/disabled): digits
//     1234567890, then !#$%&()*+ , then -=@^_.
//   - Layout 2, "symbols" (18 of 26 slots used, 8 blank/disabled): the
//     double/single quotes, comma, period, slash, colon, semicolon, angle
//     brackets and question mark, then the brackets/backslash/backtick/
//     braces/pipe/tilde left over from the 6- and 4-wide ASCII ranges.
// A "blank" slot in a layout is the same QPushButton as every other layout's
// use of that grid cell, just with empty text and setEnabled(false) --
// hidden capacity, not a destroyed widget, and it is skipped by both the
// hit-test and clicked() (Qt does not emit clicked() for a disabled button).
// Space (objectName WifiKeySpace) and Backspace (objectName
// WifiKeyBackspace) are their own fixed keys, outside the 26-slot grid,
// unaffected by layout or shift.
//
// Size budget: COMPUTED below, not measured -- CLAUDE.md's rule ("measure,
// do not compute") means these numbers are a plausibility check the
// controller must still confirm on-device with shoot.sh/measure-panel.py,
// not a substitute for that measurement. Built for the FLX6 settings page's
// body, about 1280x560-600 (BiteDJ's 480px floor cannot fit it -- see
// phase-4-brief.md -- so this widget is FLX6-only). One QGridLayout, 10
// columns x 7 rows: title (row 0), masked field (row 1), four key rows
// (rows 2-5), Cancel/Join (row 6), 8px outer margins and 6px spacing
// throughout. Title/field/Cancel/Join sit at their content height; rows 2-5
// share row stretch 1 each, and every key in every row -- not just those
// four -- has an Expanding vertical QSizePolicy (QPushButton's default is
// Fixed, which would otherwise leave the key stuck at its ~44px sizeHint
// with a dead band below it for the rest of a taller row -- Fix round 1).
// So the four key rows actually fill whatever height the page gives this
// widget, growing past 44px on the real ~580px-tall page, rather than the
// 360-410px figure a naive sum of sizeHints would suggest. Every key still
// carries an explicit setMinimumSize(44, 44) as the floor under that
// growth, for whatever height the widget ends up with.
//
// Keys call WifiSettings::appendPasswordChar/backspacePassword/submitJoin/
// cancelJoin directly -- no per-key ControlObjects (47 of them would be 47
// chances for check-log.sh's silent-missing-ObjectName failure mode). The
// plaintext never touches this class beyond the one QChar passed straight
// into appendPasswordChar: it is never stored in a member, never put in the
// masked field (which only ever holds passwordLength() U+2022 bullets), and
// never logged. tryInstance() == nullptr is inert: the keypad, title and
// field still render and the keys can still be tapped (Shift/layout
// switching keep working locally), but nothing reaches a backend that
// doesn't exist, and no crash results.
//
// Because a test harness must not construct the real WifiSettings (its
// constructor schedules a real nmcli call), every key's action is mirrored
// on a signal emitted unconditionally, the same pattern WWifiList's
// rowActivated() uses for taps: characterTyped(QChar) for every character
// key including Space, backspaceTyped() for Backspace, cancelRequested()
// and joinRequested() for the bottom row. A test builds this widget without
// the singleton and asserts on those signals and on button text/enabled/
// dynamic-property state; it never has to inspect WifiSettings.
//
// objectNames a skin can style: WifiKeypadTitle, WifiKeypadField, WifiKey
// (all 26 letter/digit/symbol keys), WifiKeyShift, WifiKeyLayout,
// WifiKeyBackspace, WifiKeySpace, WifiKeypadCancel, WifiKeypadJoin. Two
// dynamic bool properties, both restyled with unpolish/polish on change (the
// same idiom WWifiList uses for its rows' "active"/"saved" properties):
//   - "shiftActive" on WifiKeyShift: Shift's engaged state.
//   - "down" on WHICHEVER button (any of the above, not just WifiKey) is
//     currently under an in-progress press: set true in mousePressEvent,
//     cleared in mouseReleaseEvent, on a drag that leaves the pressed key
//     (mouseMoveEvent), on a QEvent::TouchCancel, and on hideEvent. This is
//     the touch equivalent of Qt's own `:pressed` pseudo-state, which never
//     fires here because a child key never receives the press that would
//     trigger it (Fix round 1 / Ruling 21).
class WWifiKeypad : public WWidget {
    Q_OBJECT
  public:
    explicit WWifiKeypad(QWidget* parent = nullptr);
    ~WWifiKeypad() override = default;

    void setup(const QDomNode& node, const SkinContext& context);

  signals:
    // Emitted for every character a tap or click commits, Space included,
    // whether or not the WifiSettings singleton exists -- see the header
    // comment for why a test needs this. Alongside it, when the singleton
    // does exist, this class calls WifiSettings::appendPasswordChar(c).
    // WARNING: this carries one plaintext password character. It exists
    // ONLY so a test built without the WifiSettings singleton can observe
    // what a tap committed. Production code (skin connections, other
    // widgets, logging) must never connect to it -- WifiSettings is the
    // only place this plaintext is allowed to go.
    void characterTyped(QChar c);
    // Mirrors a Backspace commit the same way, alongside
    // WifiSettings::backspacePassword().
    void backspaceTyped();
    // Mirrors a Cancel commit, alongside WifiSettings::cancelJoin().
    void cancelRequested();
    // Mirrors a Join commit, alongside WifiSettings::submitJoin().
    void joinRequested();

  protected:
    // See the header comment: the whole gesture is handled here because,
    // on this touchscreen, none of the child key buttons ever receive it.
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    // Only overridden to catch QEvent::TouchCancel: WWidget::event() (see
    // wwidget.cpp) translates TouchBegin/Update/End into synthesized mouse
    // events but has no case for TouchCancel, so without this a cancelled
    // gesture would leave m_pressedKeyIndex and a key's "down" property
    // stuck. Everything else is forwarded to WWidget::event() unchanged.
    bool event(QEvent* e) override;
    // Returns to a clean state whenever the page hides (Cancel back to the
    // list, a wrong-password bounce, or just navigating away): abc layout,
    // Shift off, no stuck press/"down". A DJ returning to page 1 should
    // never find Shift armed or a symbol layout left over from before.
    void hideEvent(QHideEvent* e) override;

  private slots:
    void onJoinTargetChanged(const QString& ssid);
    void onPasswordChanged(int length);
    // Desktop/mouse path: a real QMouseEvent reaches the child button
    // directly and emits clicked(). On the touchscreen this never fires;
    // the press/release handlers above do the dispatching instead.
    void onKeyClicked();

  private:
    // Index of the visible, enabled button under globalPos within
    // m_allButtons, or -1 for none. Mirrors WUsbList::buttonIndexAt.
    int keyIndexAt(const QPoint& globalPos) const;
    // The single place a committed key (from either input path) is handled:
    // figures out which button this is (a fixed special key, or a slot in
    // the 26-key grid) and acts on it.
    void dispatchButton(QPushButton* pButton);
    // Recomputes every character-slot button's text/enabled state and
    // m_currentChars for the current layout and shift state, and refreshes
    // the layout-switch key's own label. Called on construction and after
    // every layout switch or Shift toggle.
    void refreshLayout();
    void toggleShift();
    void cycleLayout();
    // Emits characterTyped(c) and calls WifiSettings::appendPasswordChar(c)
    // if the singleton exists. The low-level primitive; does not touch
    // Shift. Used directly by nothing outside commitTypedChar() today, kept
    // separate from it so a future caller that must not perturb Shift (there
    // isn't one yet) has somewhere to attach.
    void commitChar(QChar c);
    // commitChar(c), then consumes a pending one-shot Shift if this was a
    // letter typed in the "abc" layout. Used by both the 26-key grid and
    // Space -- Space is a typed character and arms/disarms Shift like any
    // other. Backspace does NOT go through this: see the header comment.
    void commitTypedChar(QChar c);
    void commitBackspace();
    void commitCancel();
    void commitJoin();
    // Sets/clears the "down" dynamic property (see the header comment) on
    // m_allButtons[index], restyling only if the value actually changed.
    // No-op for an out-of-range index, so callers can pass along
    // m_pressedKeyIndex (which is -1 when nothing is pressed) unguarded.
    void setKeyDown(int index, bool down);

    QLabel* m_pTitleLabel;
    QLabel* m_pFieldLabel;
    QGridLayout* m_pLayout;

    // The 26-slot character grid, in row-major order matching the layout
    // tables in the .cpp: Q-row (10), A-row (9), Z-row (7).
    QList<QPushButton*> m_charButtons;
    // m_charButtons[i]'s character in the CURRENT layout/shift state, or a
    // null QChar for a slot that layout leaves blank/disabled. What a
    // char-slot commit actually sends.
    QList<QChar> m_currentChars;

    QPushButton* m_pShiftButton;
    QPushButton* m_pBackspaceButton;
    QPushButton* m_pLayoutButton;
    QPushButton* m_pSpaceButton;
    QPushButton* m_pCancelButton;
    QPushButton* m_pJoinButton;

    // Every tappable button, in a fixed order used only for the outer
    // widget's hit-testing (keyIndexAt/press/release). Never rebuilt.
    QList<QPushButton*> m_allButtons;

    // 0 = letters ("abc"), 1 = digits/symbols ("123"), 2 = the rest
    // ("symbols"). See the header comment for what each holds.
    int m_currentLayout;
    // One-shot: sends the next letter uppercase, then clears itself.
    // Meaningless outside layout 0, and forced false by every layout switch.
    bool m_shiftActive;
    // Set on press, consumed on release: the key index a release must match
    // for a commit to happen. -1 when no press is pending or it did not
    // land on a key.
    int m_pressedKeyIndex;
};
