#pragma once
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

// ── A read-only, copyable report pane ───────────────────────────────────────────────────────────
//
// One definition, because two tabs ask the same question ("explain this material") and a report
// they each rendered their own way would drift in exactly the manner this codebase keeps having to
// undo. Deliberately plain text rather than a table: these reports are short arguments about where
// each fact came from, and the useful thing to do with one is paste it somewhere.
//
// Header-only and in util/ rather than folded into ViewportPartMenu.h, which five translation
// units include — ModelsTab.cpp already needed /bigobj, and pushing the whole widgets stack into
// a header that wide would spend compile budget on files that never show a report.
namespace TextReport {

inline void show(QWidget* parent, const QString& title, const QString& text)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(title);
    dlg.resize(760, 560);
    auto* lay = new QVBoxLayout(&dlg);
    auto* te  = new QPlainTextEdit(text, &dlg);
    te->setReadOnly(true);
    // Monospace: the report is aligned in columns, and a proportional font turns a table of
    // measurements back into prose.
    te->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    te->setLineWrapMode(QPlainTextEdit::NoWrap);
    lay->addWidget(te);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    auto* copy = bb->addButton(QStringLiteral("Copy"), QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, &dlg,
                     [text] { QGuiApplication::clipboard()->setText(text); });
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    dlg.exec();
}

}   // namespace TextReport
