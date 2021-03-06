#include "networkmanager.h"
#include "util/crypt.h"

NetworkManager::NetworkManager(SessionManager& sessionManager):mSessionManager(sessionManager)
{
    init();
}

NetworkManager::~NetworkManager()
{
    stop();
}

void NetworkManager::init()
{
    mIsStart = false;
    mTimeout = 10000;
    qRegisterMetaType<QAbstractSocket::SocketError>("SocketError");
    QObject::connect(&mUdpSocket,SIGNAL(readyRead()),this,SLOT(onRecvFrom()),Qt::QueuedConnection);
    QObject::connect(&mTcpServer,SIGNAL(newConnection()),this,SLOT(onAccept()),Qt::QueuedConnection);
    QObject::connect(&mTimer,SIGNAL(timeout()),this,SLOT(onTimeout()),Qt::QueuedConnection);
}

time_t NetworkManager::currentTime()
{
    return QDateTime::currentDateTime().toTime_t();
}

bool NetworkManager::isStart()
{
    return mIsStart;
}

bool NetworkManager::start(int port)
{
    if(isStart()) return false;
    mIsStart = true;
    if(!(mUdpSocket.bind(port) && mTcpServer.listen(QHostAddress::Any, port) ))
    {
        stop();
        return false;
    }
    mUdpSocket.open(QIODevice::ReadWrite);
    mTimer.start(1000);
    return true;
}

void NetworkManager::stop()
{
    mIsStart = false;
    mUdpSocket.close();
    mTcpServer.close();
    mTimer.stop();

    QMutexLocker locker(&mSessionInfoMapMutex);
    mSessionMap.clear();
}

void NetworkManager::onTimeout()
{
    handleTimeoutSessions();
    int poolSize = getHostPool().size();
    getHostPool().cleanTimeoutItem();
    if(poolSize != getHostPool().size())
    {
        emit onHostPoolChange();
    }
}

void NetworkManager::onIncomeHost(const HostInfo& hostInfo)
{
    int poolSize = getHostPool().size();
    getHostPool().putItem(hostInfo);
    if(poolSize != getHostPool().size())
    {
        emit onHostPoolChange();
        emit onHostIncome(hostInfo);
    }
}

void NetworkManager::onRecvFrom()
{
    QHostAddress host;
    quint16 port;
    QByteArray datagram;
    datagram.resize(mUdpSocket.pendingDatagramSize());
    mUdpSocket.readDatagram(datagram.data(),datagram.size(), &host, &port);
    QDataStream dataStream(datagram);
    short signature = 0;
    dataStream >> signature;
    if( signature != SIGNATURE )
    {
        return;
    }
    short operation = 0;
    dataStream >> operation;
    if(operation == OPERATION_ACK)
    {
        QByteArray uuid(datagram);
        uuid.remove(0, sizeof(signature) + sizeof(operation) );
        QMutexLocker locker(&mSessionInfoMapMutex);
        map<QString, SessionInfo>::iterator it = mSessionMap.find(uuid);
        if(it==mSessionMap.end())
        {
            return;
        }
        if(it->second.status == OPERATION_SYN)
        {
            it->second.status = OPERATION_ACK;
        }
        else
        {
            return;
        }
        HostInfo hostInfo = it->second.hostInfo;
        locker.unlock();

        QByteArray data;
        QDataStream sendDataStream(&data, QIODevice::WriteOnly);
        sendDataStream.setByteOrder(QDataStream::BigEndian);
        sendDataStream << SIGNATURE << (short)( (hostInfo.mode == HostInfo::REFLECT_CONNECT) ? OPERATION_CONNECT_HOST : OPERATION_LISTEN_HOST ); //signature & operation code
        sendDataStream.writeRawData(uuid.data(),uuid.length());
        mUdpSocket.writeDatagram(data,hostInfo.addr,hostInfo.port);
    }
    else if(operation == OPERATION_HEARTBEAT)
    {
        QByteArray info(datagram);
        info.remove(0, sizeof(signature) + sizeof(operation));
        crypt().decrypt(info.data(),info.length());
        HostInfo hostInfo;
        hostInfo.addr = host;
        hostInfo.port = port;
        hostInfo.info = info;
        hostInfo.mode = HostInfo::REFLECT_CONNECT;
        onIncomeHost(hostInfo);
    }
    else if(operation == OPERATION_ACCEPT_HOST)
    {
        short listenPort = 0;
        dataStream >> listenPort;
        if(listenPort == 0)
        {
            return;
        }
        QByteArray uuid(datagram);
        uuid.remove(0, sizeof(signature) + sizeof(operation) + sizeof(listenPort));
        QMutexLocker locker(&mSessionInfoMapMutex);
        map<QString, SessionInfo>::iterator it = mSessionMap.find(uuid);
        if(it==mSessionMap.end())
        {
            return;
        }
        locker.unlock();
        QTcpSocket *socket = new QTcpSocket();
        handleNewSocket(socket);
        socket->connectToHost(host,listenPort);
    }
    else
    {
        ;
    }
}

void NetworkManager::onAccept()
{
    handleNewSocket(mTcpServer.nextPendingConnection());
}

void NetworkManager::handleNewSocket(QAbstractSocket *socket)
{
    DataPack* dataPack = new DataPack(socket);
    QObject::connect(socket,SIGNAL(destroyed()),dataPack,SLOT(deleteLater()),Qt::QueuedConnection);
    QObject::connect(dataPack,SIGNAL(onReadData(QByteArray,DataPack*)),this,SLOT(onNewSocket(QByteArray,DataPack*)),Qt::QueuedConnection);
    QObject::connect(socket,SIGNAL(error(QAbstractSocket::SocketError)),socket,SLOT(deleteLater()),Qt::QueuedConnection);
    QObject::connect(socket,SIGNAL(aboutToClose()),socket,SLOT(deleteLater()),Qt::QueuedConnection);
}

HostPool& NetworkManager::getHostPool()
{
    return mHostPool;
}

void NetworkManager::onNewSocket(const QByteArray& data, DataPack* dataPack)
{
    QAbstractSocket* socket = dataPack->socket();
    QObject::disconnect(dataPack,SIGNAL(onReadData(QByteArray,DataPack*)),this,SLOT(onNewSocket(QByteArray,DataPack*)));
    QObject::disconnect(socket,SIGNAL(error(QAbstractSocket::SocketError)),socket,SLOT(deleteLater()));
    QObject::disconnect(socket,SIGNAL(aboutToClose()),socket,SLOT(deleteLater()));

    QMutexLocker locker(&mSessionInfoMapMutex);
    map<QString, SessionInfo>::iterator it = mSessionMap.find(data);
    if(it==mSessionMap.end())
    {
        socket->close();
        return;
    }
    //create session
    Session session = Session(this,socket,
                              it->second.hostInfo,
                              it->second.sessionName,it->second.sessionData);

    //remove session from map
    mSessionMap.erase(it);
    locker.unlock();

    //start session success
    emit onStartSessionSuccess(session.getSessionName(), session.getHostInfo());

    //session startup process
    dataPack->writeDataPack(session.getSessionName().toLocal8Bit());

    dataPack->deleteLater();

    //give session to handler
    mSessionManager.handleSession(session);
}

void NetworkManager::sendSynPack(const HostInfo& hostInfo, const QByteArray& sessionUuid)
{
    QByteArray data;
    QDataStream dataStream(&data, QIODevice::WriteOnly);
    dataStream.setByteOrder(QDataStream::BigEndian);
    dataStream << SIGNATURE << (short)( OPERATION_SYN ); //signature & operation code
    dataStream.writeRawData(sessionUuid.data(),sessionUuid.length());
    mUdpSocket.writeDatagram(data,hostInfo.addr,hostInfo.port);
}

void NetworkManager::startSession(const HostInfo& hostInfo, const QString& sessionName, const QVariant& sessionData)
{
    if(hostInfo.mode != HostInfo::REFLECT_CONNECT || hostInfo.mode != HostInfo::REFLECT_CONNECT) return;

    SessionInfo info;
    info.sessionData = sessionData;
    info.sessionName = sessionName;
    info.hostInfo = hostInfo;
    info.createTime = currentTime();
    info.status = OPERATION_SYN;

    QByteArray sessionUuid = QUuid::createUuid().toByteArray();

    QMutexLocker locker(&mSessionInfoMapMutex);
    mSessionMap[sessionUuid] = info;
    locker.unlock();

    sendSynPack(hostInfo,sessionUuid);
}

void NetworkManager::handleTimeoutSessions()
{
    QMutexLocker locker(&mSessionInfoMapMutex);

    map<QString,SessionInfo>::iterator it = mSessionMap.begin();
    while(it!=mSessionMap.end())
    {
        if( (currentTime() - it->second.createTime)*1000 >= mTimeout )
        {
            //start session failed
            emit onStartSessionFailed(it->second.sessionName, it->second.hostInfo);
            //remove it
            mSessionMap.erase(it++);
        }
        else if (it->second.status == OPERATION_SYN)
        {
            sendSynPack(it->second.hostInfo, it->first.toLocal8Bit());
            ++it;
        }
    }
}

time_t NetworkManager::getTimeout() const
{
    return mTimeout;
}

void NetworkManager::setTimeout(time_t value)
{
    mTimeout = value;
}
