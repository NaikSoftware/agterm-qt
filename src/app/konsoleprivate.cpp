#include "konsoleprivate.h"

#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QScrollBar>
#include <QStringList>
#include <QWidget>

#include <dlfcn.h>

// the plugin loads libkonsoleprivate privately, so symbols are found through the library holding a Konsole meta-object
static void *symbol(const QObject *konsoleObject, const char *mangled)
{
    Dl_info info{};
    if (!konsoleObject || !dladdr(konsoleObject->metaObject(), &info) || !info.dli_fname)
        return nullptr;
    void *lib = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    void *sym = lib ? dlsym(lib, mangled) : nullptr;
    if (lib)
        dlclose(lib); // NOLOAD only took a reference
    return sym;
}

template<typename F>
static F fn(const QObject *o, const char *mangled)
{
    return reinterpret_cast<F>(symbol(o, mangled));
}

namespace KonsolePrivate {

QObject *emulation(QObject *session)
{
    auto get = fn<QObject *(*)(const QObject *)>(session, "_ZNK7Konsole7Session9emulationEv");
    QObject *e = get ? get(session) : nullptr;
    return e && e->inherits("Konsole::Emulation") ? e : nullptr;
}

// The view's ScreenWindow is parentless and unreachable, so we read through a window of our own on the same emulation;
// the selection lives on the Screen, which every window shares. The emulation deletes its windows.
static QObject *ownWindow(QObject *session)
{
    static QHash<QObject *, QPointer<QObject>> windows;
    if (QPointer<QObject> w = windows.value(session))
        return w;
    QObject *e = emulation(session);
    auto create = fn<QObject *(*)(QObject *)>(e, "_ZN7Konsole9Emulation12createWindowEv");
    QObject *w = create ? create(e) : nullptr;
    windows.insert(session, w);
    return w;
}

QString scrollback(QObject *session, QWidget *display, bool *ok)
{
    // Session::getAllDisplayedText reads the lines the view shows, so paging the view's scrollbar walks the history
    *ok = false;
    QScrollBar *bar = display ? display->findChild<QScrollBar *>() : nullptr;
    if (!bar)
        return {};
    const int was = bar->value(), page = qMax(1, bar->pageStep());
    QStringList out;
    for (int top = 0;; top = qMin(top + page, bar->maximum())) {
        bar->setValue(top);
        QStringList lines;
        if (!QMetaObject::invokeMethod(session, "getAllDisplayedTextList", Q_RETURN_ARG(QStringList, lines), Q_ARG(bool, true)))
            break;
        out = out.mid(0, bar->value()) + lines;
        if (top >= bar->maximum()) {
            *ok = true;
            break;
        }
    }
    bar->setValue(was);
    return out.join(QLatin1Char('\n'));
}

QString selectedText(QObject *session)
{
    QObject *w = ownWindow(session);
    auto text = fn<QString (*)(const QObject *, int)>(w, "_ZNK7Konsole12ScreenWindow12selectedTextE6QFlagsINS_6Screen14DecodingOptionEE");
    return w && text ? text(w, 0x2 | 0x8) /* PreserveLineBreaks | TrimTrailingWhitespace */ : QString();
}

bool usesMouseTracking(QWidget *display, bool *ok)
{
    auto get = fn<bool (*)(const QObject *)>(display, "_ZNK7Konsole15TerminalDisplay17usesMouseTrackingEv");
    *ok = get && display->inherits("Konsole::TerminalDisplay");
    return *ok && get(display);
}

}
