#include "mainwindow.h"
#include "reader.h"
#include "ui_mainwindow.h"

#include <QAudioInput>
#include <QBuffer>
#include <QCloseEvent>
#include <QDateTime>
#include <QDebug>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <fstream>
#include <lsl_cpp.h>
#include <string>
#include <vector>

lsl::channel_format_t bits2fmt(int bits) {
	if (bits == 8) return lsl::cf_int8;
	if (bits == 16) return lsl::cf_int16;
	if (bits == 32) return lsl::cf_float32;
	// if (bits == 64) return lsl::cf_double64;
	throw std::runtime_error("Unsupported sample bits.");
}

MainWindow::MainWindow(QWidget *parent, const char *config_file)
	: QMainWindow(parent), ui(new Ui::MainWindow),
	  devices(QAudioDeviceInfo::availableDevices(QAudio::Mode::AudioInput)) {
	if(devices.empty()) {
		QMessageBox::warning(this, "Fatal error", "No capture devices found, quitting.");
		exit(1);
	}
	ui->setupUi(this);
	connect(ui->actionLoad_Configuration, &QAction::triggered, [this]() {
		load_config(QFileDialog::getOpenFileName(
			this, "Load Configuration File", "", "Configuration Files (*.cfg)"));
	});
	connect(ui->actionSave_Configuration, &QAction::triggered, [this]() {
		save_config(QFileDialog::getSaveFileName(
			this, "Save Configuration File", "", "Configuration Files (*.cfg)"));
	});
	connect(ui->actionQuit, &QAction::triggered, this, &MainWindow::close);
	connect(ui->actionAbout, &QAction::triggered, [this]() {
		QString infostr = QStringLiteral("LSL library version: ") +
						  QString::number(lsl::library_version()) +
						  "\nLSL library info:" + lsl::library_info();
		QMessageBox::about(this, "About this app", infostr);
	});
	connect(ui->linkButton, &QPushButton::clicked, this, &MainWindow::toggleRecording);

	// audio devices
	for (auto info : devices) ui->input_device->addItem(info.deviceName());
	auto changeSignal = static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged);
	connect(ui->input_device, changeSignal, this, &MainWindow::deviceChanged);
	deviceChanged();
	connect(ui->btn_checkfmt, &QPushButton::clicked, this, &MainWindow::checkAudioFormat);

	QString cfgfilepath = find_config_file(config_file);
	load_config(cfgfilepath);
}

QAudioDeviceInfo MainWindow::currentDeviceInfo() {
	return devices.at(ui->input_device->currentIndex());
}

void MainWindow::deviceChanged() {
	auto info = currentDeviceInfo();
	updateComboBoxItems(ui->input_channels, info.supportedChannelCounts());
	updateComboBoxItems(ui->input_samplerate, info.supportedSampleRates());
	updateComboBoxItems(ui->input_samplesize, info.supportedSampleSizes());
	QAudioFormat fmt(info.preferredFormat());
	if ((fmt.sampleSize() == 8 || fmt.sampleSize() == 24) && info.supportedSampleSizes().contains(16)) fmt.setSampleSize(16);
	setFmt(fmt);
}

QAudioFormat MainWindow::selectedAudioFormat() {
	auto info = currentDeviceInfo();
	QAudioFormat fmt(info.preferredFormat());
	fmt.setByteOrder(QAudioFormat::LittleEndian);
	fmt.setSampleType(QAudioFormat::SampleType::SignedInt);
	qInfo() << "Preferred: " << fmt;
	fmt.setSampleRate(ui->input_samplerate->currentText().toInt());
	fmt.setSampleSize(ui->input_samplesize->currentText().toInt());
	fmt.setChannelCount(ui->input_channels->currentText().toInt());
	return fmt;
}

void MainWindow::setFmt(const QAudioFormat &fmt) {
	qInfo() << "Setting fmt: " << fmt;
	ui->input_samplerate->setCurrentText(QString::number(fmt.sampleRate()));
	ui->input_samplesize->setCurrentText(QString::number(fmt.sampleSize()));
	ui->input_channels->setCurrentText(QString::number(fmt.channelCount()));
	auto fmtStr = QStringLiteral("%1 channels, %2 bit @ %3 Hz")
					  .arg(fmt.channelCount())
					  .arg(fmt.sampleSize())
					  .arg(fmt.sampleRate());
	ui->label_fmtresult->setText(fmtStr);
}

void MainWindow::checkAudioFormat() {
	auto fmt = selectedAudioFormat();
	auto info = currentDeviceInfo();
	if (info.isFormatSupported(fmt))
		qInfo() << "Format is supported";
	else {
		QMessageBox::warning(this, "Format not supported",
			"The requested format isn't supported; a supported format was automatically selected.");
		fmt = info.nearestFormat(fmt);
	}
	setFmt(fmt);
}

void MainWindow::updateComboBoxItems(QComboBox *box, QList<int> values) {
	const int lastValue = box->currentText().toInt();
	box->clear();
	for (int value : values) {
		box->addItem(QString::number(value));
		if (lastValue == value) box->setCurrentIndex(box->count() - 1);
	}
}

void MainWindow::load_config(const QString &filename) {
	QSettings settings(filename, QSettings::Format::IniFormat);
	ui->input_name->setText(settings.value("AudioCapture/name", "MyAudioStream").toString());
	// ui->input_device->setValue(settings.value("AudioCapture/device", 0).toInt());
}

void MainWindow::save_config(const QString &filename) {
	QSettings settings(filename, QSettings::Format::IniFormat);
	settings.beginGroup("AudioCapture");
	settings.setValue("name", ui->input_name->text());
	settings.setValue("device", ui->input_device->currentText());
	settings.sync();
}

void MainWindow::closeEvent(QCloseEvent *ev) {
	if (reader) {
		QMessageBox::warning(this, "Recording still running", "Can't quit while recording");
		ev->ignore();
	}
}

void MainWindow::toggleRecording() {
	if (!reader) {
		// read the configuration from the UI fields
		std::string name = ui->input_name->text().toStdString();
		auto fmt = selectedAudioFormat();
		int channel_count = fmt.channelCount();
		int samplerate = fmt.sampleRate();
		auto channel_format = bits2fmt(fmt.sampleSize());

		std::string stream_id = currentDeviceInfo().deviceName().toStdString();

		lsl::stream_info info(name, "Audio", channel_count, samplerate, channel_format, stream_id);
		info.desc().append_child("provider").append_child_value("api", "QtMultimedia");
		info.desc().append_child_value("device", ui->input_device->currentText().toStdString());

		audiodev = std::make_unique<QAudioInput>(currentDeviceInfo(), fmt, this);
		auto buffer_ms = ui->input_buffersize->value();
		audiodev->setBufferSize(fmt.bytesForDuration(2 * buffer_ms * 1000));
		reader = std::make_unique<LslPusher>(lsl::stream_outlet(info));
		reader->open(QIODevice::OpenModeFlag::WriteOnly);

		audiodev->start(&*reader);
		qInfo() << audiodev->state() << ' ' << audiodev->error();
		ui->linkButton->setText("Unlink");
	} else {
		qInfo() << "Read " << reader->pos() << " bytes, " <<
				   reader->samples_written() << " samples, " <<
				   ((double) reader->samples_written()/audiodev->format().sampleRate()) << 's';
		audiodev->stop();
		qInfo() << audiodev->state() << ' ' << audiodev->error();
		reader->close();
		audiodev = nullptr;
		reader = nullptr;
		ui->linkButton->setText("Link");
	}
}


/**
 * Find a config file to load. This is (in descending order or preference):
 * - a file supplied on the command line
 * - [executablename].cfg in one the the following folders:
 *	- the current working directory
 *	- the default config folder, e.g. '~/Library/Preferences' on OS X
 *	- the executable folder
 * @param filename	Optional file name supplied e.g. as command line parameter
 * @return Path to a found config file
 */
QString MainWindow::find_config_file(const char *filename) {
	if (filename) {
		QString qfilename(filename);
		if (!QFileInfo::exists(qfilename))
			QMessageBox(QMessageBox::Warning, "Config file not found",
				QStringLiteral("The file '%1' doesn't exist").arg(qfilename), QMessageBox::Ok,
				this);
		else
			return qfilename;
	}
	QFileInfo exeInfo(QCoreApplication::applicationFilePath());
	QString defaultCfgFilename(exeInfo.completeBaseName() + ".cfg");
	QStringList cfgpaths;
	cfgpaths << QDir::currentPath()
			 << QStandardPaths::standardLocations(QStandardPaths::ConfigLocation) << exeInfo.path();
	for (auto path : cfgpaths) {
		QString cfgfilepath = path + QDir::separator() + defaultCfgFilename;
		if (QFileInfo::exists(cfgfilepath)) return cfgfilepath;
	}
	QMessageBox(QMessageBox::Warning, "No config file not found",
		QStringLiteral("No default config file could be found"), QMessageBox::Ok, this);
	return "";
}


MainWindow::~MainWindow() noexcept = default;
