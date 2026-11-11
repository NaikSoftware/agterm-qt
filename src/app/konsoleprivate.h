#pragma once

#include <QString>

class QObject;
class QWidget;

// Konsole internals the part exposes no API for, reached through libkonsoleprivate's exported symbols.
// Every helper takes a Konsole::Session and degrades to an empty/failed answer when a symbol is missing.
namespace KonsolePrivate {
QObject *emulation(QObject *session);
// the whole history plus the screen, paged through the view (display) and scrolled back; empty on failure
QString scrollback(QObject *session, QWidget *display, bool *ok);
QString selectedText(QObject *session);
// whether the program in the view asked for mouse reports; *ok is false when the symbol is missing
bool usesMouseTracking(QWidget *display, bool *ok);
}
