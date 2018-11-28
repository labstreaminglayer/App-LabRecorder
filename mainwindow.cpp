#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDateTime>
#include <QDebug>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>

#include <string>
#include <vector>

// recording class
#include "recording.h"

MainWindow::MainWindow(QWidget *parent, const char *config_file)
	: QMainWindow(parent), ui(new Ui::MainWindow) {

	ui->setupUi(this);
	connect(ui->actionQuit, &QAction::triggered, this, &MainWindow::close);
	connect(ui->actionLoadConfig, &QAction::triggered, [this]() {
		this->load_config(QFileDialog::getOpenFileName(
			this, "Load Configuration File", "", "Configuration Files (*.cfg)"));
	});
	connect(ui->actionSaveConfig, &QAction::triggered, [this]() {
		this->save_config(QFileDialog::getSaveFileName(
			this, "Save Configuration File", "", "Configuration Files (*.cfg)"));
	});

	// Signals for stream finding/selecting/starting/stopping
	connect(ui->refreshButton, &QPushButton::clicked, this, &MainWindow::refreshStreams);
	connect(ui->selectAllButton, &QPushButton::clicked, this, &MainWindow::selectAllStreams);
	connect(ui->selectNoneButton, &QPushButton::clicked, this, &MainWindow::selectNoStreams);
	connect(ui->startButton, &QPushButton::clicked, this, &MainWindow::startRecording);
	connect(ui->stopButton, &QPushButton::clicked, this, &MainWindow::stopRecording);
	connect(ui->actionAbout, &QAction::triggered, [this]() {
		QString infostr = QStringLiteral("LSL library version: ") +
						  QString::number(lsl::library_version()) +
						  "\nLSL library info:" + lsl::lsl_library_info();
		QMessageBox::about(this, "About LabRecorder", infostr);
	});

	// Wheenver locationEdit is changed, print the final result.
	connect(ui->locationEdit, &QLineEdit::textChanged, this, &MainWindow::printReplacedFilename);
	connect(ui->experimentNumberSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::printReplacedFilename);
	connect(ui->spinBox_run, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::printReplacedFilename);

	// Signals for builder-related edits -> buildFilename
	connect(ui->rootBrowseButton, &QPushButton::clicked, [this]() {
		this->ui->rootEdit->setText(QFileDialog::getExistingDirectory(this, "Study root folder..."));
		this->buildFilename();
	});
	connect(ui->rootEdit, &QLineEdit::editingFinished, this, &MainWindow::buildFilename);
	connect(ui->lineEdit_participant, &QLineEdit::editingFinished, this, &MainWindow::buildFilename);
	connect(ui->lineEdit_session, &QLineEdit::editingFinished, this, &MainWindow::buildFilename);
	connect(ui->lineEdit_acq, &QLineEdit::editingFinished, this, &MainWindow::buildFilename);

	load_config(config_file);

	timer = std::make_unique<QTimer>(this);
	connect(&*timer, &QTimer::timeout, this, &MainWindow::statusUpdate);
	timer->start(1000);
	// startTime = (int)lsl::local_clock();
}

MainWindow::~MainWindow() noexcept = default;

void MainWindow::statusUpdate() const {
	if (currentRecording) {
		auto elapsed = static_cast<unsigned int>(lsl::local_clock() - startTime);
		QString recFilename = replaceFilename(ui->locationEdit->text());
		auto fileinfo = QFileInfo(recFilename);
		fileinfo.refresh();
		auto size = fileinfo.size();
		QString timeString = QStringLiteral("Recording to %1 (%2); %3kb)")
								 .arg(fileinfo.fileName(),
									 QDateTime::fromTime_t(elapsed).toUTC().toString("hh:mm:ss"),
									 QString::number(size / 1000));
		statusBar()->showMessage(timeString);
	}
}

void MainWindow::closeEvent(QCloseEvent *ev) {
	if (currentRecording) ev->ignore();
}

void MainWindow::blockSelected(const QString &block) {
	if (currentRecording)
		QMessageBox::information(this, "Still recording",
			"Please stop recording before switching blocks.", QMessageBox::Ok);
	else {
		printReplacedFilename();
		// scripted action code here...
	}
}

void MainWindow::load_config(QString filename) {
	qInfo() << "loading config file " << filename;
	try {
		// if (!QFileInfo::exists(filename)) throw std::runtime_error("Settings file doesn't
		// exist.");
		QSettings pt(filename, QSettings::Format::IniFormat);

		// ----------------------------
		// required streams
		// ----------------------------
		requiredStreams = pt.value("RequiredStreams").toStringList();

		// ----------------------------
		// online sync streams
		// ----------------------------
		QStringList onlineSyncStreams = pt.value("OnlineSync", "").toStringList();
		for (QString &oss : onlineSyncStreams) {
			QStringList words = oss.split(' ', QString::SkipEmptyParts);
			// The first two words ("StreamName (PC)") are the stream identifier
			if (words.length() < 2) {
				qInfo() << "Invalid sync stream config: " << oss;
				continue;
			}
			QString key = words.takeFirst() + ' ' + words.takeFirst();

			int val = 0;
			for (const auto &word : words) {
				if (word == "post_clocksync") { val |= lsl::post_clocksync; }
				if (word == "post_dejitter") { val |= lsl::post_dejitter; }
				if (word == "post_monotonize") { val |= lsl::post_monotonize; }
				if (word == "post_threadsafe") { val |= lsl::post_threadsafe; }
				if (word == "post_ALL") { val = lsl::post_ALL; }
			}
			syncOptionsByStreamName[key.toStdString()] = val;
			qInfo() << "stream sync options: " << key << ": " << val;
		}

		// ----------------------------
		// Block/Task Names
		// ----------------------------
		// Clear out any widgets where block/task name goes.
		QLayoutItem *child = ui->horizontalLayout_blockTask->takeAt(1);
		if (child != 0) { delete child; }
		QStringList taskNames;
		if (pt.contains("SessionBlocks")) { taskNames = pt.value("SessionBlocks").toStringList(); }
		hasTasks = taskNames.count() > 0;
		if (hasTasks) {
			taskBox = new QComboBox();
			taskBox->addItems(taskNames);
			connect(taskBox,
				static_cast<void (QComboBox::*)(const QString &)>(&QComboBox::activated), this,
				&MainWindow::blockSelected);
			ui->horizontalLayout_blockTask->addWidget(taskBox);

		} else {
			taskEdit = new QLineEdit();
			connect(taskEdit, &QLineEdit::editingFinished, this, &MainWindow::printReplacedFilename);
			ui->horizontalLayout_blockTask->addWidget(taskEdit);
		}

		// StudyRoot
		if (pt.contains("StudyRoot")) { ui->rootEdit->setText(pt.value("StudyRoot").toString()); }

		// StorageLocation
		QString str_path = pt.value("StorageLocation", "").toString();
		ui->locationEdit->setText(str_path);
		if (str_path.length() == 0) { buildFilename(); }

		// Check the wild-card-replaced filename to see if it exists already.
		// If it does then increment the exp number.
		// We only do this on settings-load because manual spin changes might indicate purposeful overwriting.
		QString recFilename = ui->locationEdit->text();
		// Spin Number
		if (recFilename.contains(QStringLiteral("%n"))) {
			for (int i = 1; i < 1001; i++) {
				ui->experimentNumberSpin->setValue(i);
				if (!QDir(replaceFilename(recFilename)).exists()) { break; }
			}
		}
		
	} catch (std::exception &e) { qWarning() << "Problem parsing config file: " << e.what(); }
	// std::cout << "refreshing streams ..." <<std::endl;
	refreshStreams();
}

void MainWindow::save_config(QString filename) {
	QSettings settings(filename, QSettings::Format::IniFormat);
	settings.setValue("StorageLocation", ui->locationEdit->text());
	qInfo() << requiredStreams;
	settings.setValue("RequiredStreams", requiredStreams);
	// Stub.
}

QSet<QString> MainWindow::getCheckedStreams() const {
	QSet<QString> checked;
	for (int i = 0; i < ui->streamList->count(); i++) {
		QListWidgetItem *item = ui->streamList->item(i);
		if (item->checkState() == Qt::Checked) checked.insert(item->text());
	}
	return checked;
}

/**
 * @brief MainWindow::refreshStreams Find streams, generate a list of missing streams
 * and fill the UI streamlist.
 * @return A vector of found stream_infos
 */
std::vector<lsl::stream_info> MainWindow::refreshStreams() {
	std::vector<lsl::stream_info> resolvedStreams = lsl::resolve_streams(1.0);

	QSet<QString> foundStreamNames;
	for (auto &s : resolvedStreams)
		foundStreamNames.insert(QString::fromStdString(s.name() + " (" + s.hostname() + ")"));

	const QSet<QString> previouslyChecked = getCheckedStreams();
	// Missing streams: all checked or required streams that weren't found
	missingStreams = (previouslyChecked + requiredStreams.toSet()) - foundStreamNames;

	// (Re-)Populate the UI list
	const QBrush good_brush(QColor(0, 128, 0)), bad_brush(QColor(255, 0, 0));
	ui->streamList->clear();
	for (auto &&streamName : foundStreamNames + missingStreams) {
		auto *item = new QListWidgetItem(streamName, ui->streamList);

		item->setCheckState(previouslyChecked.contains(streamName) ? Qt::Checked : Qt::Unchecked);
		item->setForeground(missingStreams.contains(streamName) ? bad_brush : good_brush);

		ui->streamList->addItem(item);
	}

	return resolvedStreams;
}

void MainWindow::startRecording() {

	if (!currentRecording) {

		// automatically refresh streams
		const std::vector<lsl::stream_info> resolvedStreams = refreshStreams();
		const QSet<QString> checked = getCheckedStreams();

		// if a checked stream is now missing
		// change to "checked.intersects(missingStreams) as soon as Ubuntu 16.04/Qt 5.5 is EOL
		QSet<QString> missing = checked;
		if (!missing.intersect(missingStreams).isEmpty()) {
			// are you sure?
			QMessageBox msgBox(QMessageBox::Warning, "Stream not found",
				"At least one of the streams that you checked seems to be offline",
				QMessageBox::Yes | QMessageBox::No, this);
			msgBox.setInformativeText("Do you want to start recording anyway?");
			msgBox.setDefaultButton(QMessageBox::No);
			if (msgBox.exec() != QMessageBox::Yes) return;
		}

		if (checked.isEmpty()) {
			QMessageBox msgBox(QMessageBox::Warning, "No streams selected",
				"You have selected no streams", QMessageBox::Yes | QMessageBox::No, this);
			msgBox.setInformativeText("Do you want to start recording anyway?");
			msgBox.setDefaultButton(QMessageBox::No);
			if (msgBox.exec() != QMessageBox::Yes) return;
		}

		// Handle wildcards in filename.
		QString recFilename = replaceFilename(ui->locationEdit->text());
		if (recFilename.isEmpty()) {
			QMessageBox::critical(this, "Filename empty", "Can not record without a file name");
			return;
		}

		QFileInfo recFileInfo(recFilename);
		if (recFileInfo.exists()) {
			if (recFileInfo.isDir()) {
				QMessageBox::warning(
					this, "Error", "Recording path already exists and is a directory");
				return;
			}
			QString rename_to = recFileInfo.absolutePath() + '/' + recFileInfo.baseName() +
								"_old%1." + recFileInfo.suffix();
			// search for highest _oldN
			int i = 1;
			while (QFileInfo::exists(rename_to.arg(i))) i++;
			QString newname = rename_to.arg(i);
			if (!QFile::rename(recFileInfo.absoluteFilePath(), newname)) {
				QMessageBox::warning(this, "Permissions issue",
					"Can not rename the file " + recFilename + " to " + recFileInfo.path() + '/' +
						newname);
				return;
			}
			qInfo() << "Moved existing file to " << newname;
			recFileInfo.refresh();
		}

		// regardless, we need to create the directory if it doesn't exist
		if (!recFileInfo.dir().mkpath(".")) {
			QMessageBox::warning(this, "Permissions issue",
				"Can not create the directory " + recFileInfo.dir().path() +
					". Please check your permissions.");
			return;
		}

		// go through all the listed streams
		std::vector<lsl::stream_info> checkedStreams;

		for (const lsl::stream_info &stream : resolvedStreams)
			if (checked.contains(
					QString::fromStdString(stream.name() + " (" + stream.hostname() + ')')))
				checkedStreams.push_back(stream);

		std::vector<std::string> watchfor;
		for (const QString &missing : missingStreams) watchfor.push_back(missing.toStdString());
		qInfo() << "Missing: " << missingStreams;

		currentRecording = std::make_unique<recording>(
			recFilename.toStdString(), checkedStreams, watchfor, syncOptionsByStreamName, true);
		ui->stopButton->setEnabled(true);
		ui->startButton->setEnabled(false);
		startTime = (int)lsl::local_clock();

	} else
		QMessageBox::information(
			this, "Already recording", "The recording is already running", QMessageBox::Ok);
}

void MainWindow::stopRecording() {

	if (!currentRecording)
		QMessageBox::information(
			this, "Not recording", "There is not ongoing recording", QMessageBox::Ok);
	else {
		try {
			currentRecording = nullptr;
		} catch (std::exception &e) { qWarning() << "exception on stop: " << e.what(); }
		ui->startButton->setEnabled(true);
		ui->stopButton->setEnabled(false);
		statusBar()->showMessage("Stopped");
	}
}

void MainWindow::selectAllStreams() {
	for (int i = 0; i < ui->streamList->count(); i++) {
		QListWidgetItem *item = ui->streamList->item(i);
		item->setCheckState(Qt::Checked);
	}
}

void MainWindow::selectNoStreams() {
	for (int i = 0; i < ui->streamList->count(); i++) {
		QListWidgetItem *item = ui->streamList->item(i);
		item->setCheckState(Qt::Unchecked);
	}
}

void MainWindow::buildFilename() {
	// This function is only called when a widget within Location Builder is activated.

	// Fill root folder with default if needed.
	if (ui->rootEdit->text().isEmpty()) {
			QStringList fileparts =
				QStringList(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
				<< "CurrentStudy";
			ui->rootEdit->setText(QDir::toNativeSeparators(fileparts.join(QDir::separator())));
	}

	// Build the file location in parts, starting with the root folder.
	QStringList fileparts = QStringList(ui->rootEdit->text());

	bool use_bids = (!ui->lineEdit_participant->text().isEmpty()) ||
					(!ui->lineEdit_session->text().isEmpty()) ||
					(!ui->lineEdit_acq->text().isEmpty());
	if (use_bids) {
		// path/to/CurrentStudy/sub-%p/ses-%s/eeg/sub-%p_ses-%s_task-%b[_acq-%a]_run-%r_eeg.xdf
		
		// Make sure the BIDS required fields are full.
		if (ui->lineEdit_participant->text().isEmpty()) { ui->lineEdit_participant->setText("P001"); }
		if (ui->lineEdit_session->text().isEmpty()) { ui->lineEdit_session->setText("S001"); }
		if (!hasTasks && taskEdit->text().isEmpty()) { taskEdit->setText("Default"); }
		
		// Folder hierarchy
		fileparts << "sub-%p"
				  << "ses-%s"
				  << "eeg";

		// filename
		QString fname = "sub-%p_ses-%s_task-%b";
		if (!ui->lineEdit_acq->text().isEmpty()) { fname.append("_acq-%a"); }
		fname.append("_run-%r_eeg.xdf");
		fileparts << fname;
	} else // !use_bids == use legacy
	{
		// Legacy takes the form path/to/study/exp%n/%b.xdf
		fileparts << "exp%n";

		QString fname = "";
		if (hasTasks) {
			fname.append("%b.xdf");
		} else {
			fname.append("untitled.xdf");
		}
		fileparts << fname;
	}
	QString recFilename = QDir::toNativeSeparators(fileparts.join(QDir::separator()));

	// Auto-increment Spin/Run Number if necessary.
	if (recFilename.contains(QStringLiteral("%n"))) {
		for (int i = 1; i < 1001; i++) {
			ui->experimentNumberSpin->setValue(i);
			if (!QDir(replaceFilename(recFilename)).exists()) { break; }
		}
	}
	if (recFilename.contains(QStringLiteral("%r"))) {
		for (int i = 1; i < 1001; i++) {
			ui->spinBox_run->setValue(i);
			if (!QDir(replaceFilename(recFilename)).exists()) { break; }
		}
	}
	// Sometimes locationEdit doesn't change so printReplacedFilename isn't triggered.
	// So trigger manually.
	if (ui->locationEdit->text() == recFilename) { printReplacedFilename(); }
	ui->locationEdit->setText(recFilename);
}

QString MainWindow::replaceFilename(QString fullfile) const {
	// Replace wildcards.
	// There are two different wildcard formats: legacy, BIDS

	// Legacy takes the form path/to/study/exp%n/%b.xdf
	// Where %n will be replaced by the contents of the experimentNumberSpin widget
	// and %b will be replaced by the contents of the blockList widget.
	fullfile.replace("%n", QString("%1").arg(ui->experimentNumberSpin->value(), 3, 10, QChar('0')));
	if (hasTasks) {
		fullfile.replace("%b", taskBox->currentText());
	} else {
		fullfile.replace("%b", taskEdit->text());
	}
	
	// BIDS
	// See https://docs.google.com/document/d/1ArMZ9Y_quTKXC-jNXZksnedK2VHHoKP3HCeO5HPcgLE/
	// path/to/study/sub-<participant_label>/ses-<session_label>/eeg/sub-<participant_label>_ses-<session_label>_task-<task_label>[_acq-<acq_label>]_run-<run_index>_eeg.xdf
	// path/to/study/sub-%p/ses-%s/eeg/sub-%p_ses-%s_task-%b[_acq-%a]_run-%r_eeg.xdf
	// %b already replaced above.
	fullfile.replace("%p", ui->lineEdit_participant->text());
	fullfile.replace("%s", ui->lineEdit_session->text());
	fullfile.replace("%a", ui->lineEdit_acq->text());
	fullfile.replace("%r", QString("%1").arg(ui->spinBox_run->value(), 3, 10, QChar('0')));

	return fullfile;
}

void MainWindow::printReplacedFilename() {
	ui->locationLabel->setText(replaceFilename(ui->locationEdit->text()));
}
