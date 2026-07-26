#pragma once

class QAbstractItemView;

// d4analyzer 2.5 parity: install Ctrl+C on a table/tree view so the selected rows
// (or all rows if none selected) are copied to the clipboard as CSV — header plus
// every column, RFC-4180 quoted.
namespace CsvCopy {
void install(QAbstractItemView* view);
}
