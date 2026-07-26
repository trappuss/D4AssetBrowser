#include "app/LogConsole.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMutexLocker>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

LogBuffer& LogBuffer::instance()
{
    static LogBuffer buf;
    return buf;
}

void LogBuffer::append(const QString& line)
{
    {
        QMutexLocker lk(&m_mtx);
        m_lines.append(line);
        if (m_lines.size() > 5000)
            m_lines.remove(0, m_lines.size() - 5000);   // cap memory
    }
    emit appended(line);   // queued to the GUI thread if a window is connected
}

QString LogBuffer::contents() const
{
    QMutexLocker lk(&m_mtx);
    return m_lines.join(QLatin1Char('\n'));
}

ConsoleWindow::ConsoleWindow(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Console — log output"));
    setWindowFlag(Qt::Window, true);   // independent top-level (non-modal)
    resize(820, 440);

    auto* lay = new QVBoxLayout(this);
    auto* top = new QHBoxLayout();
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(QStringLiteral("Filter (substring)…"));
    m_filter->setClearButtonEnabled(true);
    top->addWidget(m_filter, 1);
    auto* copy  = new QPushButton(QStringLiteral("Copy all"), this);
    auto* clear = new QPushButton(QStringLiteral("Clear"), this);
    top->addWidget(copy);
    top->addWidget(clear);
    lay->addLayout(top);

    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setMaximumBlockCount(5000);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_text->setStyleSheet(QStringLiteral(
        "QPlainTextEdit{font-family:Consolas,'Courier New',monospace;font-size:11px;"
        "background:#121212;color:#cccccc;}"));
    lay->addWidget(m_text, 1);

    m_text->setPlainText(LogBuffer::instance().contents());

    connect(clear, &QPushButton::clicked, m_text, &QPlainTextEdit::clear);
    connect(copy, &QPushButton::clicked, this,
            [this] { QApplication::clipboard()->setText(m_text->toPlainText()); });
    connect(m_filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    // Live append (only lines passing the current filter).
    connect(&LogBuffer::instance(), &LogBuffer::appended, this, [this](const QString& l) {
        const QString f = m_filter->text();
        if (f.isEmpty() || l.contains(f, Qt::CaseInsensitive))
            m_text->appendPlainText(l);
    });
}

void ConsoleWindow::applyFilter()
{
    const QString f = m_filter->text();
    const QString all = LogBuffer::instance().contents();
    if (f.isEmpty()) { m_text->setPlainText(all); return; }
    QStringList kept;
    for (const QString& l : all.split(QLatin1Char('\n')))
        if (l.contains(f, Qt::CaseInsensitive)) kept << l;
    m_text->setPlainText(kept.join(QLatin1Char('\n')));
}
