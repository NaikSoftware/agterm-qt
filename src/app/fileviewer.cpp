#include "fileviewer.h"
#include "filetree.h"

#include <KSyntaxHighlighting/Definition>
#include <KSyntaxHighlighting/Repository>
#include <KSyntaxHighlighting/SyntaxHighlighter>
#include <KSyntaxHighlighting/Theme>

#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QMimeDatabase>
#include <QPainter>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QTextBlock>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace {
// A QPlainTextEdit with a line-number gutter on its left, numbers in the text colour at half strength.
class CodeView : public QPlainTextEdit
{
public:
    explicit CodeView(const QString &text)
        : QPlainTextEdit(text), m_gutter(new QWidget(this))
    {
        m_gutter->installEventFilter(this);
        connect(this, &QPlainTextEdit::updateRequest, m_gutter, qOverload<>(&QWidget::update));
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QPlainTextEdit::resizeEvent(event);
        layoutGutter();
    }

    void changeEvent(QEvent *event) override
    {
        QPlainTextEdit::changeEvent(event);
        if (event->type() == QEvent::FontChange) // Ctrl+wheel zoom changes the font, not the size
            layoutGutter();
    }

    void layoutGutter()
    {
        const int digit = fontMetrics().horizontalAdvance(QLatin1Char('9'));
        const int width = digit * int(QString::number(blockCount()).size() + 2);
        setViewportMargins(width, 0, 0, 0);
        const QRect area = contentsRect();
        m_gutter->setGeometry(area.x(), area.y(), width, area.height());
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_gutter || event->type() != QEvent::Paint)
            return QPlainTextEdit::eventFilter(watched, event);
        QPainter painter(m_gutter);
        painter.fillRect(m_gutter->rect(), palette().color(QPalette::Base));
        QColor number = palette().color(QPalette::Text);
        number.setAlphaF(0.5); // not PlaceholderText: some platform themes leave it near the background
        painter.setPen(number);
        const int right = m_gutter->width() - fontMetrics().horizontalAdvance(QLatin1Char('9'));
        for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
            const int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
            if (top > m_gutter->height())
                break;
            painter.drawText(0, top, right, fontMetrics().height(), Qt::AlignRight, QString::number(block.blockNumber() + 1));
        }
        return true;
    }

private:
    QWidget *m_gutter;
};

// Kate's highlighting: the definition by file name (or by sniffed MIME type for an extensionless script),
// coloured with the bundled theme that fits the palette, whose background and text the editor takes too.
void highlight(QPlainTextEdit *edit, const QString &path)
{
    using namespace KSyntaxHighlighting;
    // shortcut: a big or minified file shows plain, since a whole-document highlight blocks the GUI for seconds
    // (4 MiB of C++: +2.3 s); upgrade to highlighting only the visible blocks if those files want colour.
    const QTextDocument *document = edit->document();
    if (document->characterCount() > (1 << 20))
        return;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next())
        if (block.length() > 10000) // Kate's own long-line limit
            return;
    static const auto *repository = new Repository; // never freed: a live highlighter's Definition depends on it
    Definition definition = repository->definitionForFileName(path);
    if (!definition.isValid())
        definition = repository->definitionForMimeType(QMimeDatabase().mimeTypeForFile(path).name());
    if (!definition.isValid())
        return;
    const Theme theme = repository->themeForPalette(edit->palette());
    QPalette palette = edit->palette();
    palette.setColor(QPalette::Base, QColor::fromRgba(theme.editorColor(Theme::BackgroundColor)));
    palette.setColor(QPalette::Text, QColor::fromRgba(theme.textColor(Theme::Normal)));
    edit->setPalette(palette);
    auto *highlighter = new SyntaxHighlighter(edit->document());
    highlighter->setTheme(theme);
    highlighter->setDefinition(definition);
}
}

bool FileViewer::open(const QString &path, QWidget *parent)
{
    QWidget *content = nullptr;
    QPixmap pixmap;
    switch (FileTree::viewerFor(path)) {
    case FileTree::Viewer::Markdown: {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        auto *browser = new QTextBrowser;
        browser->document()->setBaseUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath() + QLatin1Char('/')));
        browser->setOpenExternalLinks(true);
        browser->setMarkdown(QString::fromUtf8(file.readAll()));
        content = browser;
        break;
    }
    case FileTree::Viewer::Image:
        if (!pixmap.load(path))
            return false;
        break;
    case FileTree::Viewer::Text: {
        const QString text = FileTree::readText(path);
        if (text.isNull())
            return false;
        auto *edit = new CodeView(text);
        edit->setReadOnly(true);
        edit->setLineWrapMode(QPlainTextEdit::NoWrap);
        edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        highlight(edit, path);
        content = edit;
        break;
    }
    case FileTree::Viewer::External:
        return false;
    }
    auto *viewer = new FileViewer(path, parent);
    if (content) {
        viewer->layout()->addWidget(content);
    } else {
        viewer->m_pixmap = pixmap;
        viewer->m_image = new QLabel;
        viewer->m_image->setAlignment(Qt::AlignCenter);
        viewer->m_image->setMinimumSize(1, 1);
        viewer->layout()->addWidget(viewer->m_image);
        viewer->resize(pixmap.size().boundedTo(QSize(1200, 900)).expandedTo(QSize(320, 240)));
    }
    viewer->show();
    return true;
}

FileViewer::FileViewer(const QString &path, QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QFileInfo(path).fileName());
    setWindowFilePath(path);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    resize(860, 680);
    new QShortcut(Qt::Key_Escape, this, this, &QWidget::close);
}

void FileViewer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_image)
        return;
    // Shown at its own size in logical pixels, shrunk to fit but never enlarged.
    const qreal dpr = devicePixelRatioF();
    QPixmap px = m_pixmap.scaled(m_pixmap.size().boundedTo(m_image->size()) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    px.setDevicePixelRatio(dpr);
    m_image->setPixmap(px);
}
