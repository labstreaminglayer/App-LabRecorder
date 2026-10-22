#include "mainwindow.h"

#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

void requireOnly(const std::vector<lsl::stream_info> &streams, const std::string &id) {
	require(streams.size() == 1, "Expected exactly one selected stream after refresh");
	require(streams.front().source_id() == id, "Refresh selected the wrong source ID");
}

void writeConfig(const QString &path, const QString &root, const QStringList &required = {}) {
	QSettings config(path, QSettings::IniFormat);
	config.setValue("StudyRoot", root);
	config.setValue("PathTemplate", "selection-test.xdf");
	config.setValue("RCSEnabled", false);
	config.setValue("AutoStart", false);
	config.setValue("RequiredStreams", required);
	config.sync();
	require(config.status() == QSettings::NoError, "Cannot write temporary recorder config");
}
} // namespace

// Access is limited to this executable; no QtTest or alternate selection implementation.
class StreamSelectionTest {
public:
	static void sourceIds(const QString &configPath) {
		lsl::stream_outlet anonymous(lsl::stream_info("SelectionTwin", "Test", 1, 0,
			lsl::cf_float32, ""));
		lsl::stream_outlet identified(lsl::stream_info("SelectionTwin", "Test", 1, 0,
			lsl::cf_float32, "different-source"));
		MainWindow window(nullptr, configPath.toUtf8().constData());
		const auto matches = lsl::resolve_stream("name='SelectionTwin'", 2, 5.0);
		require(matches.size() == 2, "Could not discover both test outlets");

		// Resolver order is unspecified. Exercise both orders of the existing GUI rows.
		for (bool reverse : {false, true}) {
			window.findChild<QListWidget *>("streamList")->clear();
			window.knownStreams.clear();
			window.knownStreams << StreamItem(matches[reverse ? 1 : 0], false)
				<< StreamItem(matches[reverse ? 0 : 1], false);
			window.rebuildStreamList();
			window.selectNoStreams();
			require(window.selectStreams("source_id='different-source'") ==
				MainWindow::SelectResult::Selected, "Source-ID query did not select a stream");
			requireOnly(window.refreshStreams(), "different-source");
		}

		// Invalid and unmatched queries must preserve the existing selection.
		require(window.selectStreams("name='absent'") == MainWindow::SelectResult::NoMatches,
			"Unmatched query returned the wrong result");
		requireOnly(window.refreshStreams(), "different-source");
		require(window.selectStreams("name='unterminated") == MainWindow::SelectResult::InvalidQuery,
			"Malformed query returned the wrong result");
		requireOnly(window.refreshStreams(), "different-source");
	}

	static void missingSelection(const QString &configPath, const QString &root) {
		lsl::stream_outlet intended(lsl::stream_info("SelectionIntended", "Test", 1, 0,
			lsl::cf_float32, "intended"));
		const QString missing = "SelectionMissing (" +
			QString::fromStdString(intended.info().hostname()) + ")";
		writeConfig(configPath, root, {missing});
		MainWindow window(nullptr, configPath.toUtf8().constData());
		require(window.selectedMissingStreams().contains(missing),
			"Configured missing stream should initially be selected");
		window.selectNoStreams();
		require(!window.hasSelectedStreams(), "Select none left a stream selected");
		window.refreshStreams();
		require(!window.hasSelectedStreams(), "Refresh reselected a missing stream");
		window.load_config(configPath);
		require(window.selectedMissingStreams().contains(missing),
			"Loading config did not restore its required-stream defaults");
		window.selectNoStreams();
		require(window.selectStreams("source_id='intended'") == MainWindow::SelectResult::Selected,
			"Intended stream query did not match");
		requireOnly(window.refreshStreams(), "intended");
		const auto rows = window.findChild<QListWidget *>("streamList")
			->findItems(missing, Qt::MatchExactly);
		require(rows.size() == 1 && rows.front()->checkState() == Qt::Unchecked,
			"Query/rebuild lost the missing stream's unchecked state");
		require(window.selectedMissingStreamQueries().empty(),
			"An unchecked missing stream would enter the recording watchlist");
		window.save_config(root + "/saved.cfg");
		QSettings saved(root + "/saved.cfg", QSettings::IniFormat);
		require(!saved.value("RequiredStreams").toStringList().contains(missing),
			"Saving config restored an unchecked missing stream as required");

		window.selectAllStreams();
		window.refreshStreams();
		require(window.selectedMissingStreams().contains(missing),
			"Select all failed to restore the missing stream to the watchlist");
		require(window.selectedMissingStreamQueries().size() == 1,
			"Checked required stream was omitted from the watchlist");
		window.selectNoStreams();
		window.selectStreams("source_id='intended'");
		lsl::stream_outlet late(lsl::stream_info("SelectionMissing", "Test", 1, 0,
			lsl::cf_float32, "excluded-late"));
		require(lsl::resolve_stream("source_id='excluded-late'", 1, 5.0).size() == 1,
			"Could not discover the late test outlet");
		requireOnly(window.refreshStreams(), "intended");
	}

	static void reconnectIdentity(const QString &configPath) {
		auto sourceA = std::make_unique<lsl::stream_outlet>(lsl::stream_info(
			"ReconnectTwin", "Test", 1, 0, lsl::cf_float32, "source-A"));
		MainWindow window(nullptr, configPath.toUtf8().constData());
		window.selectNoStreams();
		require(window.selectStreams("source_id='source-A'") == MainWindow::SelectResult::Selected,
			"Source A was not selected");
		requireOnly(window.refreshStreams(), "source-A");
		const auto originalInfo = sourceA->info();
		sourceA.reset();
		require(window.refreshStreams().empty(), "Source A should be offline");
		require(window.selectedMissingStreams().size() == 1, "Lost selected source A while offline");
		lsl::stream_outlet sourceB(lsl::stream_info(
			"ReconnectTwin", "Test", 1, 0, lsl::cf_float32, "source-B"));
		require(lsl::resolve_stream("source_id='source-B'", 1, 5.0).size() == 1,
			"Could not discover source B");
		require(window.refreshStreams().empty(), "Source B replaced selected source A");
		require(window.selectedMissingStreams().size() == 1, "Source B removed missing source A");
		const auto queries = window.selectedMissingStreamQueries();
		require(queries.size() == 1 && originalInfo.matches_query(queries.front().c_str()) &&
			!sourceB.info().matches_query(queries.front().c_str()),
			"Recording watchlist no longer requires source A's identity");
		sourceA = std::make_unique<lsl::stream_outlet>(lsl::stream_info(
			"ReconnectTwin", "Test", 1, 0, lsl::cf_float32, "source-A"));
		require(lsl::resolve_stream("source_id='source-A'", 1, 5.0).size() == 1,
			"Could not rediscover source A");
		requireOnly(window.refreshStreams(), "source-A");
		sourceA.reset();
		window.refreshStreams();
		window.selectStreams("source_id='source-B'");
		require(window.selectedMissingStreams().size() == 1,
			"Selecting source B erased the pending selection of source A");
	}

	static void watchlistIdentity(const QString &configPath) {
		MainWindow window(nullptr, configPath.toUtf8().constData());
		// Also exercise metadata quoting and the empty-ID fallback without live discovery.
		for (const std::string id : {std::string(), std::string("source-'\"A")}) {
			lsl::stream_info original("Quoted '\" stream", "Test", 1, 0, lsl::cf_float32, id);
			lsl::stream_info other("Quoted '\" stream", "Test", 1, 0, lsl::cf_float32, "other");
			window.missingStreams = {
				MissingStreamItem{"same label", true, StreamItem(original, true)},
				MissingStreamItem{"same label", true, StreamItem(other, true)}};
			window.rebuildStreamList();
			window.findChild<QListWidget *>("streamList")->item(1)->setCheckState(Qt::Unchecked);
			window.updateStreamSelectionFromUi();
			const auto queries = window.selectedMissingStreamQueries();
			require(queries.size() == 1 && original.matches_query(queries.front().c_str()) &&
				!other.matches_query(queries.front().c_str()),
				"Missing rows with equal labels lost their independent identity/selection");
		}
	}
};

int main(int argc, char **argv) {
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);
	QTemporaryDir temp;
	if (!temp.isValid()) return 1;
	// Isolate discovery from other experiments before creating any liblsl object.
	QFile lslConfig(temp.filePath("lsl_api.cfg"));
	if (!lslConfig.open(QIODevice::WriteOnly)) return 1;
	lslConfig.write("[lab]\nSessionID=" + QUuid::createUuid().toByteArray(QUuid::WithoutBraces) +
		"\nKnownPeers={127.0.0.1}\n[multicast]\nResolveScope=machine\n");
	lslConfig.close();
	qputenv("LSLAPICFG", lslConfig.fileName().toUtf8());
	const QString configPath = temp.filePath("LabRecorder.cfg");
	int failures = 0;
	const auto run = [&](const char *name, auto test) {
		try {
			writeConfig(configPath, temp.path());
			test();
			std::cout << "PASS: " << name << '\n';
		} catch (const std::exception &error) {
			std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
			++failures;
		}
	};
	run("source IDs after refresh", [&] { StreamSelectionTest::sourceIds(configPath); });
	run("missing stream selection", [&] { StreamSelectionTest::missingSelection(configPath, temp.path()); });
	run("reconnect identity", [&] { StreamSelectionTest::reconnectIdentity(configPath); });
	run("watchlist identity", [&] { StreamSelectionTest::watchlistIdentity(configPath); });
	return failures ? 1 : 0;
}
