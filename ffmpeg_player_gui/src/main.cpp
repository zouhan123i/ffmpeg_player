#include <QApplication>
#include <QLoggingCategory>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    // 屏蔽所有 Qt 警告
    QLoggingCategory::setFilterRules(QStringLiteral("*.warning=false\n"));
    QApplication app(argc, argv);
    MainWindow w;
    w.show();
    return app.exec();
} 