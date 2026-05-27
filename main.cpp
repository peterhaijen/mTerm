#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("mTerm"));
    QCoreApplication::setApplicationName(QStringLiteral("mTerm"));
    QGuiApplication::setDesktopFileName(QStringLiteral("mterm"));
    a.setWindowIcon(QIcon::fromTheme(QStringLiteral("mterm"),
                                     QIcon(QStringLiteral(":/icons/mterm.svg"))));

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "mTerm_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    MainWindow w;
    w.show();
    return a.exec();
}
