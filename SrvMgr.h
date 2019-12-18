#ifndef SRVMGR_H
#define SRVMGR_H

#include "Mgr.h"
#include "Options.h"
#include <QList>

#include <memory>

class Server;
class Storage;

class SrvMgr : public Mgr
{
    Q_OBJECT
public:
    explicit SrvMgr(const QList<Options::Interface> &interfaces, std::shared_ptr<Storage> storage, QObject *parent = nullptr);
    ~SrvMgr() override;
    void startup() override; // may throw on error
    void cleanup() override;

    int nServers() const { return servers.size(); }

public slots:

protected:
    Stats stats() const override;

private:
    void startServers();
    QList<Options::Interface> interfaces;
    QList<Server *> servers;
    std::shared_ptr<Storage> storage;
};

#endif // SRVMGR_H
