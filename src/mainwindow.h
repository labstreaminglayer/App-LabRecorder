#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include <QCloseEvent>
#include <QComboBox>
#include <QListWidget>
#include <QMainWindow>
#include <QStringList>
#include <QTimer>
#include <memory> //for std::unique_ptr

// LSL
#include <lsl_cpp.h>

namespace Ui {
class MainWindow;
}

class recording;
class RemoteControlSocket;

class StreamItem {
	
public:
	StreamItem(std::string stream_name, std::string stream_type, std::string source_id,
		std::string hostname, bool required)
		: name(stream_name), type(stream_type), id(source_id), host(hostname), checked(required) {}
	
	QString listName() { return QString::fromStdString(name + " (" + host + ")"); }
	std::string name;
	std::string type;
	std::string id;
	std::string host;
	bool checked;
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
	void selectStreams(const QString& search_str);
	void selectRegexStreams(const QString& regex_pattern);
	void selectNoStreams();
	void buildFilename();
	void buildBidsTemplate();
	void printReplacedFilename();
	void enableRcs(bool bEnable);
	void rcsCheckBoxChanged(bool checked);
	void rcsUpdateFilename(QString s);
	void rcsStartRecording();
	void rcsStopRecording();
	void rcsportValueChangedInt(int value);

private:
	QString replaceFilename(QString fullfile) const;
	// function for loading / saving the config file
	QString find_config_file(const char *filename);
	QString counterPlaceholder() const;
	void load_config(QString filename);
	void save_config(QString filename);

	std::unique_ptr<recording> currentRecording;
	std::unique_ptr<RemoteControlSocket> rcs;

	int startTime;
	std::unique_ptr<QTimer> timer;

	QList<StreamItem> knownStreams;
	QSet<QString> missingStreams;
	std::map<std::string, int> syncOptionsByStreamName;

	// QString recFilename;
	QString legacyTemplate;
	std::unique_ptr<Ui::MainWindow> ui; // window pointer

	// @Doug1983 added to suppress pop-ups when remotely starting recording
	// and missing streams or having some unchecked streams
	bool hideWarnings = false;
};

#endif // MAINWINDOW_H
