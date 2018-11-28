#ifndef MAINWINDOW_H
#define MAINWINDOW_H

// Qt
#include "ui_mainwindow.h"

#include <QComboBox>
#include <QCloseEvent>
#include <QListWidget>
#include <QMainWindow>
#include <QStringList>
#include <QTimer>
#include <memory>

// LSL
#include <lsl_cpp.h>

class recording;

class RecorderItem {

public:
	QListWidgetItem listItem;
	std::string uid;
};


class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit MainWindow(QWidget *parent, const char *config_file);
	~MainWindow() noexcept override;

private slots:
	void statusUpdate(void) const;
	void closeEvent(QCloseEvent *ev) override;
	void blockSelected(const QString &block);
	std::vector<lsl::stream_info> refreshStreams(void);
	void startRecording(void);
	void stopRecording(void);
	void selectAllStreams();
	void selectNoStreams();
	void buildFilename();
	void printReplacedFilename();

private:
	QSet<QString> getCheckedStreams() const;
	QString replaceFilename(QString fullfile) const;
	// function for loading config file
	void load_config(QString filename);
	void save_config(QString filename);

	std::unique_ptr<recording> currentRecording;

	int startTime;
	std::unique_ptr<QTimer> timer;

	QStringList requiredStreams;
	std::map<std::string, int> syncOptionsByStreamName;
	QSet<QString> missingStreams;

	//QString recFilename;
	bool hasTasks = false;
	QComboBox *taskBox;
	QLineEdit *taskEdit;

	std::unique_ptr<Ui::MainWindow> ui; // window pointer
};

#endif // MAINWINDOW_H
