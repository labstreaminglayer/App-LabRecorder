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
class QTcpSocket;

class StreamItem {
	
public:
	StreamItem(const lsl::stream_info &info, bool required)
		: name(info.name()), type(info.type()), id(info.source_id()), host(info.hostname()),
		  uid(info.uid()), sessionId(info.session_id()), channelCount(info.channel_count()),
		  nominalSrate(info.nominal_srate()), channelFormat(info.channel_format()),
		  createdAt(info.created_at()), checked(required) {}
	
	QString listName() const { return QString::fromStdString(name + " (" + host + ")"); }
	bool matches(const lsl::stream_info &info) const {
		if (!id.empty() || !info.source_id().empty())
			return name == info.name() && type == info.type() && id == info.source_id();
		return name == info.name() && type == info.type() && host == info.hostname() &&
			   sessionId == info.session_id() && channelCount == info.channel_count() &&
			   nominalSrate == info.nominal_srate() && channelFormat == info.channel_format();
	}
	void updateInfo(const lsl::stream_info &info) {
		name = info.name();
		type = info.type();
		id = info.source_id();
		host = info.hostname();
		uid = info.uid();
		sessionId = info.session_id();
		channelCount = info.channel_count();
		nominalSrate = info.nominal_srate();
		channelFormat = info.channel_format();
		createdAt = info.created_at();
	}
	std::string name;
	std::string type;
	std::string id;
	std::string host;
	std::string uid;
	std::string sessionId;
	int32_t channelCount;
	double nominalSrate;
	lsl::channel_format_t channelFormat;
	double createdAt;
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
	void selectNoStreams();
	void selectStreams(const QString &query);
	void buildFilename();
	void buildBidsTemplate();
	void printReplacedFilename();
	void enableRcs(bool bEnable);
	void rcsCheckBoxChanged(bool checked);
	void rcsUpdateFilename(QString s);
	void rcsStartRecording(QTcpSocket *sock);
	void rcsStopRecording();
	void rcsportValueChangedInt(int value);

private:
	QString replaceFilename(QString fullfile) const;
	// function for loading / saving the config file
	QString find_config_file(const char *filename);
	QString counterPlaceholder() const;
	bool hasSelectedStreams() const;
	void updateKnownStreamSelectionFromUi();
	void rebuildStreamList();
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
