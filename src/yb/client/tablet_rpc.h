//
// Copyright (c) YugaByte, Inc.
//

#ifndef YB_CLIENT_TABLET_RPC_H
#define YB_CLIENT_TABLET_RPC_H

#include <unordered_set>

#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/client_fwd.h"

#include "yb/rpc/rpc_fwd.h"
#include "yb/rpc/rpc.h"

#include "yb/tserver/tserver.pb.h"

#include "yb/util/status.h"
#include "yb/util/trace.h"

namespace yb {

namespace tserver {
class TabletServerServiceProxy;
}

namespace client {
namespace internal {

class TabletRpc {
 public:
  virtual const tserver::TabletServerErrorPB* response_error() const = 0;
  virtual void Failed(const Status& status) = 0;
  virtual void SendRpcToTserver() = 0;
 protected:
  ~TabletRpc() {}
};

class TabletInvoker {
 public:
  explicit TabletInvoker(YBClient* client,
                         rpc::RpcCommand* command,
                         TabletRpc* rpc,
                         RemoteTablet* tablet,
                         rpc::RpcRetrier* retrier,
                         Trace* trace)
      : client_(client),
        tablet_(tablet),
        command_(command),
        rpc_(rpc),
        retrier_(retrier),
        trace_(trace) {}

  virtual ~TabletInvoker() {}

  void Execute();
  bool Done(Status* status);

  bool IsLocalCall() const;
  const RemoteTablet& tablet() const { return *tablet_; }
  std::shared_ptr<tserver::TabletServerServiceProxy> proxy() const;

 protected:
  virtual void SelectTabletServer();
  YBClient* client_;

  // The tablet that should receive this rpc.
  RemoteTablet* const tablet_;

  // The TS receiving the write. May change if the write is retried.
  // RemoteTabletServer is taken from YBClient cache, so it is guaranteed that those objects are
  // alive while YBClient is alive. Because we don't delete them, but only add and update.
  RemoteTabletServer* current_ts_ = nullptr;

 private:
  // Called when we finish initializing a TS proxy.
  // Sends the RPC, provided there was no error.
  void InitTSProxyCb(const Status& status);

  // Marks all replicas on current_ts_ as failed and retries the write on a
  // new replica.
  void FailToNewReplica(const Status& reason);

  // Called when we finish a lookup (to find the new consensus leader). Retries
  // the rpc after a short delay.
  void LookupTabletCb(const Status& status);

  rpc::RpcCommand* const command_;

  TabletRpc* const rpc_;

  rpc::RpcRetrier* const retrier_;

  // Trace is provided externally and owner of this object should guarantee that it will be alive
  // while this object is alive.
  Trace* const trace_;

  // Used to retry some failed RPCs.
  // Tablet servers that refused the write because they were followers at the time.
  // Cleared when new consensus configuration information arrives from the master.
  std::unordered_set<RemoteTabletServer*> followers_;
};

// This is an implementation of ReadRpc with consistency level as CONSISTENT_PREFIX. As a result,
// there is no requirement that the read needs to hit the leader.
class ConsistentPrefixTabletInvoker : public TabletInvoker {
 public:
  explicit ConsistentPrefixTabletInvoker(YBClient* client,
                                         rpc::RpcCommand* command,
                                         TabletRpc* rpc,
                                         RemoteTablet* tablet,
                                         rpc::RpcRetrier* retrier,
                                         Trace* trace)
      : TabletInvoker(client, command, rpc, tablet, retrier, trace) {}

 protected:
  void SelectTabletServer() {
    vector<RemoteTabletServer*> candidates;
    current_ts_ = client_->data_->SelectTServer(tablet_,
                                                YBClient::ReplicaSelection::CLOSEST_REPLICA, {},
                                                &candidates);
    VLOG(1) << "Using tserver: " << current_ts_->ToString();
  }
};


CHECKED_STATUS ErrorStatus(const tserver::TabletServerErrorPB* error);
tserver::TabletServerErrorPB_Code ErrorCode(const tserver::TabletServerErrorPB* error);

} // namespace internal
} // namespace client
} // namespace yb

#endif // YB_CLIENT_TABLET_RPC_H