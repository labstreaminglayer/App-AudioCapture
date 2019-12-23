#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include "ui_mainwindow.h"
#include <QAudioDeviceInfo>
#include <QMainWindow>
#include <memory> //for std::unique_ptr

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(QWidget *parent, const char *config_file);
	~MainWindow() noexcept override;

private slots:
	void closeEvent(QCloseEvent *ev) override;
	void toggleRecording();
	void deviceChanged();
	void checkAudioFormat();

private:
	// Audio device handling
	QAudioDeviceInfo currentDeviceInfo();
	void setFmt(const QAudioFormat &fmt);
	QAudioFormat selectedAudioFormat();
	void updateSampleRates();
	void updateComboBoxItems(QComboBox *box, QList<int> values);

	// function for loading / saving the config file
	QString find_config_file(const char *filename);
	void load_config(const QString &filename);
	void save_config(const QString &filename);
	std::unique_ptr<class LslPusher> reader;
	std::unique_ptr<class QAudioInput> audiodev;
	std::unique_ptr<Ui::MainWindow> ui; // window pointer
	QList<QAudioDeviceInfo> devices;
};

#endif // MAINWINDOW_H
