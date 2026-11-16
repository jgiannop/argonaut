#pragma once

#include <QTextBrowser>

// Rich text that comes from outside (an extension, Claude), shown and
// nothing more: links don't go anywhere and nothing it names (pictures,
// style sheets) is loaded.
class RichView : public QTextBrowser
{
public:
    using QTextBrowser::QTextBrowser;

protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};
