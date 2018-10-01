#pragma once

#include <cstdint>

#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QDataStream>

class RemoteControlSocket: public QObject {
	Q_OBJECT
	QTcpServer server;
	QList<QTcpSocket*> clients;
public:
	RemoteControlSocket(uint16_t port);

signals:
	void start();
	void stop();

    public slots:
	void addClient();
	void handleLine(QString s, QTcpSocket* sock);
};
