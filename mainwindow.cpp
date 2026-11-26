#include "mainwindow.h"
#include "session.h"
#include "ui_mainwindow.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDebug>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <lsl_cpp.h>
#include <string>

// Helper to convert SampleFormat to string
QString sampleFormatToString(QAudioFormat::SampleFormat fmt) {
    switch (fmt) {
        case QAudioFormat::UInt8: return "UInt8";
        case QAudioFormat::Int16: return "Int16";
        case QAudioFormat::Int32: return "Int32";
        case QAudioFormat::Float: return "Float";
        default: return "Unknown";
    }
}

QAudioFormat::SampleFormat stringToSampleFormat(const QString &str) {
	for (auto f : {QAudioFormat::UInt8, QAudioFormat::Int16, QAudioFormat::Int32, QAudioFormat::Float})
		if (sampleFormatToString(f) == str) return f;
	return QAudioFormat::Unknown;
}

MainWindow::MainWindow(QWidget *parent, const char *config_file)
	: QMainWindow(parent), ui(new Ui::MainWindow),
	  devices(QMediaDevices::audioInputs()) {
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
	auto *poller = new QTimer(this);  // drives the capture watchdog
	connect(poller, &QTimer::timeout, this, [this] { if (session) session->poll(); });
	poller->start(500);

	// audio devices
	for (const auto &dev : devices) ui->input_device->addItem(dev.description());
	const auto changeSignal = static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged);

	connect(ui->input_device, changeSignal, this, &MainWindow::deviceChanged);
	deviceChanged();

	connect(ui->btn_checkfmt, &QPushButton::clicked, this, &MainWindow::checkAudioFormat);

	QString cfgfilepath = find_config_file(config_file);
	load_config(cfgfilepath);
	checkAudioFormat(cfgfilepath.length() > 0);
}

QAudioDevice MainWindow::currentDevice() const {
	return devices.at(ui->input_device->currentIndex());
}

void MainWindow::deviceChanged() const {
	auto const dev = currentDevice();

	// Channel count
	ui->input_channels->setMinimum(dev.minimumChannelCount());
	ui->input_channels->setMaximum(dev.maximumChannelCount());
	ui->input_channels->setValue(dev.preferredFormat().channelCount());
	// Sample rate
	ui->input_samplerate->setMinimum(dev.minimumSampleRate());
	ui->input_samplerate->setMaximum(dev.maximumSampleRate());
	ui->input_samplerate->setValue(dev.preferredFormat().sampleRate());
	ui->input_samplerate->setToolTip(
		QString("%1 - %2 Hz")
		.arg(dev.minimumSampleRate())
		.arg(dev.maximumSampleRate())
	);  // Not showing up?
	ui->input_samplerate->setToolTipDuration(5000);
	// Sample format
	ui->input_sampleformat->clear();
	for (auto samp_fmt : dev.supportedSampleFormats()) {
		ui->input_sampleformat->addItem(sampleFormatToString(samp_fmt), QVariant::fromValue(samp_fmt));
	}
	auto const fmt(dev.preferredFormat());
	if (const auto idx = ui->input_sampleformat->findData(QVariant::fromValue(fmt.sampleFormat())); idx >= 0) ui->input_sampleformat->setCurrentIndex(idx);
	setFmt(fmt);
}

QAudioFormat MainWindow::selectedAudioFormat() const {
	const auto dev = currentDevice();
	QAudioFormat fmt(dev.preferredFormat());
	qInfo() << "Preferred: " << fmt;
	fmt.setSampleRate(ui->input_samplerate->value());
	fmt.setChannelCount(ui->input_channels->value());
	// fmt.setByteOrder(QAudioFormat::LittleEndian);
	// fmt.setChannelConfig(??);
	fmt.setSampleFormat(stringToSampleFormat(ui->input_sampleformat->currentText()));
	return fmt;
}

void MainWindow::setFmt(const QAudioFormat &fmt) const {
	qInfo() << "Setting fmt: " << fmt;
	ui->input_samplerate->setValue(fmt.sampleRate());
	ui->input_sampleformat->setCurrentText(sampleFormatToString(fmt.sampleFormat()));
	ui->input_channels->setValue(fmt.channelCount());
	const auto fmtStr = QStringLiteral("%1 channels, %2 bit @ %3 Hz")
					  .arg(fmt.channelCount())
					  .arg(fmt.sampleFormat())
					  .arg(fmt.sampleRate());
	ui->label_fmtresult->setText(fmtStr);
}

void MainWindow::checkAudioFormat(const bool showModal = true) {
	auto fmt = selectedAudioFormat();
	if (const auto dev = currentDevice(); dev.isFormatSupported(fmt))
		qInfo() << "Format is supported";
	else {
		if (showModal)
			QMessageBox::warning(this, "Format not supported",
				"The requested format isn't supported; the preferred format was automatically selected.");
		fmt = dev.preferredFormat();
	}
	setFmt(fmt);
}

void MainWindow::load_config(const QString &filename) {
	const QSettings settings(filename, QSettings::Format::IniFormat);
	ui->input_name->setText(settings.value("AudioCapture/name", "MyAudioStream").toString());
	ui->input_device->setCurrentIndex(settings.value("AudioCapture/device", 0).toInt());
	ui->input_samplerate->setValue(settings.value("AudioCapture/samplerate", 1).toInt());
	const QString sampleSize = settings.value("AudioCapture/samplesize", "Int16").toString();
	ui->input_sampleformat->setCurrentIndex(ui->input_sampleformat->findText(sampleSize));
	ui->input_channels->setValue(settings.value("AudioCapture/channels", 0).toInt());
	timestamp_offset_us = settings.value("AudioCapture/timestamp_offset_us", 0.0).toDouble();
}

void MainWindow::save_config(const QString &filename) const {
	QSettings settings(filename, QSettings::Format::IniFormat);
	settings.beginGroup("AudioCapture");
	settings.setValue("name", ui->input_name->text());
	settings.setValue("device", ui->input_device->currentIndex());
	settings.setValue("samplerate", ui->input_samplerate->value());
	settings.setValue("samplesize", ui->input_sampleformat->currentText());
	settings.setValue("channels", ui->input_channels->value());
	settings.setValue("timestamp_offset_us", timestamp_offset_us);
	settings.sync();
}

void MainWindow::closeEvent(QCloseEvent *ev) {
	if (session) {
		QMessageBox::warning(this, "Recording still running", "Can't quit while recording");
		ev->ignore();
	}
}

void MainWindow::toggleRecording() {
	if (!session) {
		// read the configuration from the UI fields
		auto const name = ui->input_name->text().toStdString();
		auto const qfmt = selectedAudioFormat();
		auto const qdev = currentDevice();
		AudioFormat fmt{qfmt.channelCount(), qfmt.sampleRate()};
		if (!parseSampleFormat(sampleFormatToString(qfmt.sampleFormat()).toStdString(), fmt.format)) {
			QMessageBox::warning(this, "Capture failed", "Choose Int16, Int32 or Float; LSL has no unsigned 8-bit format.");
			return;
		}
		DeviceInfo dev{qdev.id().toStdString(), qdev.description().toStdString(), fmt};
		// Capture through the preferred API (PipeWire on Linux) when it has this device, else QtMultimedia.
		const CaptureApi *api = findCaptureApi("QtMultimedia");
		if (const auto *def = defaultCaptureApi(); def && def != api)
			for (const auto &d : def->list())
				if (d.id == dev.id || d.description == dev.description) {
					api = def;
					dev.id = d.id;
					break;
				}
		qInfo() << "Capturing via" << api->name;

		session = std::make_unique<CaptureSession>(audioStreamInfo(name, fmt, dev, api->name, timestamp_offset_us),
			timestamp_offset_us * 1e-6, [api, dev, fmt] { return api->make(dev.id, fmt); },
			[](const std::string &msg) { qWarning() << msg.c_str(); });
		if (std::string err; !session->start(err)) {
			QMessageBox::warning(this, "Capture failed", QString::fromStdString(err));
			session = nullptr;
			return;
		}
		ui->linkButton->setText("Unlink");
		statusBar()->showMessage(QStringLiteral("Streaming via %1").arg(api->name));
	} else {
		qInfo() << "Pushed" << session->samples_written() << "samples";
		session = nullptr;
		ui->linkButton->setText("Link");
		statusBar()->clearMessage();
	}
}


/**
 * Find a config file to load. This is (in descending order or preference):
 * - a file supplied on the command line
 * - [executablename].cfg in one the following folders:
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
	const QFileInfo exeInfo(QCoreApplication::applicationFilePath());
	const QString defaultCfgFilename(exeInfo.completeBaseName() + ".cfg");
	QStringList cfgpaths;
	cfgpaths << QDir::currentPath()
			 << QStandardPaths::standardLocations(QStandardPaths::ConfigLocation) << exeInfo.path();
	for (const auto& path : cfgpaths) {
		QString cfgfilepath = path + QDir::separator() + defaultCfgFilename;
		if (QFileInfo::exists(cfgfilepath)) return cfgfilepath;
	}
	QMessageBox msg_box(QMessageBox::Warning, "No config file not found",
		QStringLiteral("No default config file could be found"), QMessageBox::Ok, this);
	return "";
}


MainWindow::~MainWindow() noexcept = default;
