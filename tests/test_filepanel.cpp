#include "fileicons.h"
#include "filepanel.h"
#include "filetree.h"
#include "fileviewer.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QFontMetrics>
#include <QLabel>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeView>

using namespace FileIcons;

namespace {
void touch(const QString &path, const QByteArray &data = "x")
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(data);
}

const QStringList catalogSamples{
    QStringLiteral("Dockerfile"), QStringLiteral("Makefile"), QStringLiteral("package.json"), QStringLiteral("package-lock.json"),
    QStringLiteral("Package.swift"), QStringLiteral("yarn.lock"), QStringLiteral(".gitignore"), QStringLiteral(".env"),
    QStringLiteral("README.md"), QStringLiteral(".env.local"), QStringLiteral("Cargo.lock"), QStringLiteral("x.qqq"),
};
const QStringList catalogExtensions{
    QStringLiteral("swift"), QStringLiteral("js"), QStringLiteral("mjs"), QStringLiteral("cjs"), QStringLiteral("ts"),
    QStringLiteral("tsx"), QStringLiteral("jsx"), QStringLiteral("json"), QStringLiteral("yaml"), QStringLiteral("yml"),
    QStringLiteral("toml"), QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("txt"), QStringLiteral("sh"),
    QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("py"), QStringLiteral("go"), QStringLiteral("rs"),
    QStringLiteral("rb"), QStringLiteral("java"), QStringLiteral("kt"), QStringLiteral("c"), QStringLiteral("h"),
    QStringLiteral("cpp"), QStringLiteral("hpp"), QStringLiteral("cs"), QStringLiteral("php"), QStringLiteral("html"),
    QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("sql"), QStringLiteral("xml"), QStringLiteral("svg"),
    QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("webp"),
    QStringLiteral("pdf"), QStringLiteral("zip"), QStringLiteral("gz"), QStringLiteral("dart"), QStringLiteral("lua"),
    QStringLiteral("vue"), QStringLiteral("svelte"),
};

QModelIndex child(QTreeView *view, const QString &name, const QModelIndex &parent = {})
{
    const QModelIndex p = parent.isValid() ? parent : view->rootIndex();
    for (int r = 0; r < view->model()->rowCount(p); ++r)
        if (view->model()->index(r, 0, p).data().toString() == name)
            return view->model()->index(r, 0, p);
    return {};
}

void trigger(QTreeView *view, const QString &text)
{
    for (QAction *a : view->actions())
        if (a->text() == text)
            return a->trigger();
    QFAIL(qPrintable(text));
}
}

class TestFilePanel : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void exactNameBeatsExtension()
    {
        QCOMPARE(icon(QStringLiteral("package.json"), false), (Icon{Font::FileIcons, 0xE91C, Color::Red}));
        QCOMPARE(icon(QStringLiteral("x.json"), false), (Icon{Font::FileIcons, 0xEABE, Color::Cyan}));
        QCOMPARE(icon(QStringLiteral("yarn.lock"), false).glyph, char32_t(0xEA1A));
    }

    void catalogRules()
    {
        QCOMPARE(icon(QStringLiteral("A.SWIFT"), false), icon(QStringLiteral("a.swift"), false));
        QCOMPARE(icon(QStringLiteral("src"), true), (Icon{Font::FontAwesome, 0xF07B, Color::Grey}));
        QCOMPARE(icon(QStringLiteral("x.qqq"), false), (Icon{Font::FontAwesome, 0xF016, Color::Grey}));
        QCOMPARE(icon(QStringLiteral(".bashrc"), false).glyph, char32_t(0xF016));
        QCOMPARE(icon(QStringLiteral("Cargo.lock"), false).glyph, char32_t(0xF023));
        QCOMPARE(icon(QStringLiteral("README.md"), false).glyph, char32_t(0xF02D));
        QCOMPARE(icon(QStringLiteral(".env.local"), false), icon(QStringLiteral(".env"), false));
        QCOMPARE(icon(QStringLiteral("a.toml"), false).glyph, char32_t(0x1F143));
    }

    void everyColorHasBothHexes()
    {
        const QRegularExpression re(QStringLiteral("^#[0-9a-f]{6}$"));
        for (int c = int(Color::Red); c <= int(Color::Grey); ++c)
            for (bool dark : {true, false})
                QVERIFY(re.match(hex(Color(c), dark)).hasMatch());
    }

    void everyGlyphDrawsFromItsBundledFont()
    {
        QStringList names = catalogSamples;
        for (const QString &ext : catalogExtensions)
            names << QStringLiteral("f.") + ext;
        QList<Icon> icons{icon(QStringLiteral("d"), true)};
        for (const QString &n : names)
            icons << icon(n, false);
        for (const Icon &i : icons) {
            const QFont f = FilePanel::iconFont(i.font, 14);
            QCOMPARE(QFontInfo(f).family(), family(i.font));
            QVERIFY2(QFontMetrics(f).inFontUcs4(i.glyph), qPrintable(QString::number(i.glyph, 16)));
        }
    }

    void copyNameAddsCopySuffix()
    {
        using FileTree::copyName;
        QCOMPARE(copyName(QStringLiteral("a.txt"), false, [](auto) { return false; }), QStringLiteral("a.txt"));
        QCOMPARE(copyName(QStringLiteral("a.txt"), false, [](auto n) { return n == QLatin1String("a.txt"); }), QStringLiteral("a copy.txt"));
        const QStringList taken{QStringLiteral("a.txt"), QStringLiteral("a copy.txt")};
        QCOMPARE(copyName(QStringLiteral("a.txt"), false, [&](auto n) { return taken.contains(n); }), QStringLiteral("a copy 2.txt"));
        QCOMPARE(copyName(QStringLiteral("src.d"), true, [](auto n) { return n == QLatin1String("src.d"); }), QStringLiteral("src.d copy"));
        QCOMPARE(copyName(QStringLiteral(".env"), false, [](auto n) { return n == QLatin1String(".env"); }), QStringLiteral(".env copy"));
        QCOMPARE(copyName(QStringLiteral("a.tar.gz"), false, [](auto n) { return n == QLatin1String("a.tar.gz"); }),
                 QStringLiteral("a.tar copy.gz"));
    }

    void moveRules()
    {
        using FileTree::canMove;
        QVERIFY(!canMove(QStringLiteral("/t/a"), QStringLiteral("/t/a")));
        QVERIFY(!canMove(QStringLiteral("/t/a"), QStringLiteral("/t/a/b")));
        QVERIFY(canMove(QStringLiteral("/t/a"), QStringLiteral("/t/ab")));
        QVERIFY(canMove(QStringLiteral("/t/a"), QStringLiteral("/u")));
        QVERIFY(!canMove(QStringLiteral("/t/a/f.txt"), QStringLiteral("/t/a")));
    }

    void renameAndPasteRules()
    {
        for (const QString &bad : {QString(), QStringLiteral("  "), QStringLiteral("a/b"), QStringLiteral("a:b"), QStringLiteral(".."),
                                   QStringLiteral(".")})
            QVERIFY2(!FileTree::renameError(bad, [](auto) { return false; }).isEmpty(), qPrintable(bad));
        QVERIFY(!FileTree::renameError(QStringLiteral("taken"), [](auto n) { return n == QLatin1String("taken"); }).isEmpty());
        QVERIFY(FileTree::renameError(QStringLiteral("ok.txt"), [](auto) { return false; }).isEmpty());
        QCOMPARE(FileTree::pasteTarget(QStringLiteral("/r/d"), true, QStringLiteral("/r")), QStringLiteral("/r/d"));
        QCOMPARE(FileTree::pasteTarget(QStringLiteral("/r/d/f.txt"), false, QStringLiteral("/r")), QStringLiteral("/r/d"));
        QCOMPARE(FileTree::pasteTarget(QString(), false, QStringLiteral("/r")), QStringLiteral("/r"));
    }

    void viewerChoice()
    {
        QTemporaryDir tmp;
        using FileTree::Viewer;
        QCOMPARE(FileTree::viewerFor(QStringLiteral("a.MD")), Viewer::Markdown);
        QCOMPARE(FileTree::viewerFor(QStringLiteral("a.webp")), Viewer::Image);
        QCOMPARE(FileTree::viewerFor(QStringLiteral("a.svg")), Viewer::External);
        QCOMPARE(FileTree::viewerFor(QStringLiteral("Makefile")), Viewer::Text);
        touch(tmp.filePath(QStringLiteral("t")), "héllo");
        touch(tmp.filePath(QStringLiteral("e")), "");
        touch(tmp.filePath(QStringLiteral("bin")), QByteArray("a\0b", 3));
        touch(tmp.filePath(QStringLiteral("latin1")), "\xe9t\xe9");
        QCOMPARE(FileTree::readText(tmp.filePath(QStringLiteral("t"))), QStringLiteral("héllo"));
        QVERIFY(!FileTree::readText(tmp.filePath(QStringLiteral("e"))).isNull());
        QVERIFY(FileTree::readText(tmp.filePath(QStringLiteral("bin"))).isNull());
        QVERIFY(FileTree::readText(tmp.filePath(QStringLiteral("latin1"))).isNull());
    }

    void textViewerHighlightsSourceOnly()
    {
        QTemporaryDir tmp;
        touch(tmp.filePath(QStringLiteral("a.py")), "def f():\n    return 1\n");
        touch(tmp.filePath(QStringLiteral("run")), "#!/bin/sh\necho hi\n");
        touch(tmp.filePath(QStringLiteral("notes.qqq")), "plain words\n");
        touch(tmp.filePath(QStringLiteral("min.js")), "var a = '" + QByteArray(10000, 'x') + "';\n");
        const auto open = [&](const QString &name) -> QPlainTextEdit * {
            if (!FileViewer::open(tmp.filePath(name)))
                return nullptr;
            for (QWidget *w : QApplication::topLevelWidgets())
                if (w->windowFilePath() == tmp.filePath(name))
                    return w->findChild<QPlainTextEdit *>();
            return nullptr;
        };
        for (const QString name : {QStringLiteral("a.py"), QStringLiteral("run")}) { // by extension, by shebang
            QPlainTextEdit *edit = open(name);
            QVERIFY2(edit, qPrintable(name));
            QVERIFY2(edit->document()->findChild<QSyntaxHighlighter *>(), qPrintable(name));
            QVERIFY2(!edit->document()->firstBlock().layout()->formats().isEmpty(), qPrintable(name));
            edit->window()->close();
        }
        for (const QString name : {QStringLiteral("notes.qqq"), QStringLiteral("min.js")}) { // unknown type, a too-long line
            QPlainTextEdit *plain = open(name);
            QVERIFY2(plain, qPrintable(name));
            QVERIFY2(!plain->document()->findChild<QSyntaxHighlighter *>(), qPrintable(name));
            plain->window()->close();
        }
    }

    void transferNeverOverwrites()
    {
        QTemporaryDir tmp;
        const QString root = tmp.path(), sub = tmp.filePath(QStringLiteral("sub"));
        QDir(root).mkpath(QStringLiteral("sub/inner"));
        touch(tmp.filePath(QStringLiteral("a.txt")), "root");
        touch(tmp.filePath(QStringLiteral("sub/a.txt")), "sub");
        touch(tmp.filePath(QStringLiteral("sub/inner/f")));

        QStringList results;
        QCOMPARE(FileTree::transfer({tmp.filePath(QStringLiteral("a.txt"))}, sub, false, &results), QStringList());
        QCOMPARE(results, QStringList{tmp.filePath(QStringLiteral("sub/a copy.txt"))});
        QCOMPARE(FileTree::transfer({tmp.filePath(QStringLiteral("a.txt"))}, sub, true).size(), 1);
        QVERIFY(QFile::exists(tmp.filePath(QStringLiteral("a.txt"))));
        QFile kept(tmp.filePath(QStringLiteral("sub/a.txt")));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("sub"));

        QCOMPARE(FileTree::transfer({sub}, tmp.filePath(QStringLiteral("sub/inner")), true).size(), 1);
        QCOMPARE(FileTree::transfer({sub}, tmp.filePath(QStringLiteral("sub/inner")), false).size(), 1);
        QCOMPARE(FileTree::transfer({sub}, root, false), QStringList());
        QVERIFY(QFile::exists(tmp.filePath(QStringLiteral("sub copy/inner/f"))));
        QDir(root).mkdir(QStringLiteral("dest"));
        QCOMPARE(FileTree::transfer({sub}, tmp.filePath(QStringLiteral("dest")), true), QStringList());
        QVERIFY(QFile::exists(tmp.filePath(QStringLiteral("dest/sub/inner/f"))));

        QVERIFY(!FileTree::rename(tmp.filePath(QStringLiteral("a.txt")), QStringLiteral("dest")).isEmpty());
        QString dest;
        QVERIFY(FileTree::rename(tmp.filePath(QStringLiteral("a.txt")), QStringLiteral("b.txt"), &dest).isEmpty());
        QCOMPARE(dest, tmp.filePath(QStringLiteral("b.txt")));
    }

    void panelShowsTheFolderAndPastesWithoutOverwriting()
    {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath(QStringLiteral("z"));
        QDir(tmp.path()).mkpath(QStringLiteral(".git"));
        for (const char *name : {"a10", "a2", "b.txt", ".env"})
            touch(tmp.filePath(QString::fromLatin1(name)));
        FilePanel panel;
        panel.show();
        auto *view = panel.findChild<QTreeView *>();
        auto *empty = panel.findChildren<QLabel *>().last();
        QVERIFY(empty->isVisible());
        QCOMPARE(empty->text(), QStringLiteral("No folder"));

        panel.setRoot(tmp.path());
        QVERIFY(!empty->isVisible());
        QTRY_COMPARE(view->model()->rowCount(view->rootIndex()), 5);
        QTRY_COMPARE(view->model()->index(0, 0, view->rootIndex()).data().toString(), QStringLiteral("z"));
        QVERIFY(!child(view, QStringLiteral(".git")).isValid());
        QVERIFY(child(view, QStringLiteral("a2")).row() < child(view, QStringLiteral("a10")).row());

        view->setCurrentIndex(child(view, QStringLiteral("b.txt")));
        trigger(view, QStringLiteral("Copy"));
        trigger(view, QStringLiteral("Paste"));
        QVERIFY(QFile::exists(tmp.filePath(QStringLiteral("b copy.txt"))));
        QVERIFY(QFile::exists(tmp.filePath(QStringLiteral("b.txt"))));

        QTRY_VERIFY(view->selectionModel()->isSelected(child(view, QStringLiteral("b copy.txt"))));
        view->setCurrentIndex(child(view, QStringLiteral("b.txt")));
        trigger(view, QStringLiteral("Cut"));
        view->setCurrentIndex(child(view, QStringLiteral("z")));
        trigger(view, QStringLiteral("Paste"));
        QVERIFY(QFile::exists(tmp.filePath(QStringLiteral("z/b.txt"))));
        QVERIFY(!QFile::exists(tmp.filePath(QStringLiteral("b.txt"))));

        touch(tmp.filePath(QStringLiteral("c")));
        QTRY_VERIFY(child(view, QStringLiteral("c")).isValid());

        panel.setRoot(tmp.filePath(QStringLiteral("missing")));
        QCOMPARE(empty->text(), QStringLiteral("Folder not found"));
    }

    void switchingFoldersKeepsTheModelAndEachFoldersExpansion()
    {
        QTemporaryDir a, b;
        QDir(a.path()).mkpath(QStringLiteral("sub/deep"));
        touch(b.filePath(QStringLiteral("f")));
        FilePanel panel;
        panel.resize(300, 400);
        panel.show();
        auto *view = panel.findChild<QTreeView *>();
        panel.setRoot(a.path());
        QTRY_VERIFY(child(view, QStringLiteral("sub")).isValid());
        view->expand(child(view, QStringLiteral("sub")));
        const QAbstractItemModel *model = view->model();
        panel.setRoot(b.path());
        QCOMPARE(view->model(), model); // a session switch moves the root; it does not rebuild the tree
        QTRY_VERIFY(child(view, QStringLiteral("f")).isValid());
        panel.setRoot(a.path());
        QTRY_VERIFY(view->isExpanded(child(view, QStringLiteral("sub"))));
    }
};

QTEST_MAIN(TestFilePanel)
#include "test_filepanel.moc"
