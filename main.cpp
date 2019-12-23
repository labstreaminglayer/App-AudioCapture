#include "mainwindow.h"
#include <QApplication>
int main(int argc, char *argv[]) {
	QApplication a(argc, argv);
	MainWindow w(nullptr, argc > 1 ? argv[1] : nullptr);
	w.show();
	return a.exec();
}
