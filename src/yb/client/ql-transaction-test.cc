//
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//

#include <thread>

#include <boost/optional/optional.hpp>
#include <boost/scope_exit.hpp>

#include "yb/client/ql-dml-test-base.h"
#include "yb/client/table_handle.h"
#include "yb/client/transaction.h"
#include "yb/client/transaction_rpc.h"
#include "yb/client/transaction_manager.h"

#include "yb/consensus/consensus.h"

#include "yb/yql/cql/ql/util/errcodes.h"
#include "yb/yql/cql/ql/util/statement_result.h"

#include "yb/rpc/rpc.h"

#include "yb/server/hybrid_clock.h"
#include "yb/server/skewed_clock.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/transaction_coordinator.h"
#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/random_util.h"

using namespace std::literals;

using yb::tablet::GetTransactionTimeout;
using yb::tablet::TabletPeer;

DECLARE_uint64(transaction_heartbeat_usec);
DECLARE_double(transaction_max_missed_heartbeat_periods);
DECLARE_uint64(log_segment_size_bytes);
DECLARE_int32(log_min_seconds_to_retain);
DECLARE_bool(transaction_disable_heartbeat_in_tests);
DECLARE_double(transaction_ignore_applying_probability_in_tests);
DECLARE_uint64(transaction_check_interval_usec);
DECLARE_uint64(max_clock_skew_usec);
DECLARE_bool(transaction_allow_rerequest_status_in_tests);
DECLARE_uint64(transaction_delay_status_reply_usec_in_tests);
DECLARE_string(time_source);
DECLARE_bool(flush_rocksdb_on_shutdown);
DECLARE_bool(transaction_disable_proactive_cleanup_in_tests);
DECLARE_uint64(aborted_intent_cleanup_ms);
DECLARE_int32(intents_flush_max_delay_ms);
DECLARE_int32(remote_bootstrap_max_chunk_size);
DECLARE_int32(load_balancer_max_concurrent_adds);
DECLARE_int32(master_inject_latency_on_transactional_tablet_lookups_ms);

namespace yb {
namespace client {

namespace {

constexpr size_t kNumRows = 5;
const auto kTransactionApplyTime = NonTsanVsTsan(3s, 15s);

// We use different sign to distinguish inserted and updated values for testing.
int32_t GetMultiplier(const WriteOpType op_type) {
  switch (op_type) {
    case WriteOpType::INSERT:
      return 1;
    case WriteOpType::UPDATE:
      return -1;
    case WriteOpType::DELETE:
      return 0; // Value is not used in delete path.
  }
  FATAL_INVALID_ENUM_VALUE(WriteOpType, op_type);
}

int32_t KeyForTransactionAndIndex(size_t transaction, size_t index) {
  return static_cast<int32_t>(transaction * 10 + index);
}

int32_t ValueForTransactionAndIndex(size_t transaction, size_t index, const WriteOpType op_type) {
  return static_cast<int32_t>(transaction * 10 + index + 2) * GetMultiplier(op_type);
}

void SetIgnoreApplyingProbability(double value) {
  SetAtomicFlag(value, &FLAGS_transaction_ignore_applying_probability_in_tests);
}

void SetDisableHeartbeatInTests(bool value) {
  SetAtomicFlag(value, &FLAGS_transaction_disable_heartbeat_in_tests);
}

void DisableApplyingIntents() {
  SetIgnoreApplyingProbability(1.0);
}

void CommitAndResetSync(YBTransactionPtr *txn) {
  CountDownLatch latch(1);
  (*txn)->Commit([&latch](const Status& status) {
    ASSERT_OK(status);
    latch.CountDown(1);
  });
  txn->reset();
  latch.Wait();
}

void DisableTransactionTimeout() {
  SetAtomicFlag(std::numeric_limits<double>::max(),
                &FLAGS_transaction_max_missed_heartbeat_periods);
}

} // namespace

#define VERIFY_ROW(...) VerifyRow(__LINE__, __VA_ARGS__)

class QLTransactionTest : public KeyValueTableTest {
 protected:
  void SetUp() override {
    server::SkewedClock::Register();
    FLAGS_time_source = server::SkewedClock::kName;
    FLAGS_load_balancer_max_concurrent_adds = 100;
    KeyValueTableTest::SetUp();

    CreateTable(Transactional::kTrue);

    FLAGS_log_segment_size_bytes = log_segment_size_bytes();
    FLAGS_log_min_seconds_to_retain = 5;
    FLAGS_intents_flush_max_delay_ms = 250;

    HybridTime::TEST_SetPrettyToString(true);

    ASSERT_OK(clock_->Init());
    transaction_manager_.emplace(client_, clock_, client::LocalTabletFilter());

    server::ClockPtr clock2(new server::HybridClock(skewed_clock_));
    ASSERT_OK(clock2->Init());
    transaction_manager2_.emplace(client_, clock2, client::LocalTabletFilter());
  }

  virtual uint64_t log_segment_size_bytes() const {
    return 128;
  }

  void WriteRows(
      const YBSessionPtr& session, size_t transaction = 0,
      const WriteOpType op_type = WriteOpType::INSERT) {
    for (size_t r = 0; r != kNumRows; ++r) {
      ASSERT_OK(WriteRow(
          session,
          KeyForTransactionAndIndex(transaction, r),
          ValueForTransactionAndIndex(transaction, r, op_type),
          op_type));
    }
  }

  void VerifyRow(int line, const YBSessionPtr& session, int32_t key, int32_t value,
                 const std::string& column = kValueColumn) {
    VLOG(4) << "Calling SelectRow";
    auto row = SelectRow(session, key, column);
    ASSERT_TRUE(row.ok()) << "Bad status: " << row << ", originator: " << __FILE__ << ":" << line;
    VLOG(4) << "SelectRow returned: " << *row;
    ASSERT_EQ(value, *row) << "Originator: " << __FILE__ << ":" << line;
  }

  void WriteData(const WriteOpType op_type = WriteOpType::INSERT, size_t transaction = 0) {
    auto txn = CreateTransaction();
    WriteRows(CreateSession(txn), transaction, op_type);
    ASSERT_OK(txn->CommitFuture().get());
    LOG(INFO) << "Committed";
  }

  void WriteDataWithRepetition() {
    auto txn = CreateTransaction();
    auto session = CreateSession(txn);
    for (size_t r = 0; r != kNumRows; ++r) {
      for (int j = 10; j--;) {
        ASSERT_OK(WriteRow(
            session,
            KeyForTransactionAndIndex(0, r),
            ValueForTransactionAndIndex(0, r, WriteOpType::INSERT) + j));
      }
    }
    ASSERT_OK(txn->CommitFuture().get());
  }

  YBTransactionPtr CreateTransaction() {
    auto result = std::make_shared<YBTransaction>(transaction_manager_.get_ptr());
    EXPECT_OK(result->Init(IsolationLevel::SNAPSHOT_ISOLATION));
    return result;
  }

  YBTransactionPtr CreateTransaction2() {
    auto result = std::make_shared<YBTransaction>(transaction_manager2_.get_ptr());
    EXPECT_OK(result->Init(IsolationLevel::SNAPSHOT_ISOLATION));
    return result;
  }

  void VerifyRows(const YBSessionPtr& session,
                  size_t transaction = 0,
                  const WriteOpType op_type = WriteOpType::INSERT,
                  const std::string& column = kValueColumn) {
    std::vector<client::YBqlReadOpPtr> ops;
    for (size_t r = 0; r != kNumRows; ++r) {
      ops.push_back(ReadRow(session, KeyForTransactionAndIndex(transaction, r), column));
    }
    ASSERT_OK(session->Flush());
    for (size_t r = 0; r != kNumRows; ++r) {
      SCOPED_TRACE(Format("Row: $0, key: $1", r, KeyForTransactionAndIndex(transaction, r)));
      auto& op = ops[r];
      ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
      auto rowblock = yb::ql::RowsResult(op.get()).GetRowBlock();
      ASSERT_EQ(rowblock->row_count(), 1);
      const auto& first_column = rowblock->row(0).column(0);
      ASSERT_EQ(QLValue::InternalType::kInt32Value, first_column.type());
      ASSERT_EQ(first_column.int32_value(), ValueForTransactionAndIndex(transaction, r, op_type));
    }
  }

  YBqlReadOpPtr ReadRow(const YBSessionPtr& session,
                        int32_t key,
                        const std::string& column = kValueColumn) {
    auto op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, key);
    table_.AddColumns({column}, req);
    EXPECT_OK(session->Apply(op));
    return op;
  }

  void VerifyData(size_t num_transactions = 1, const WriteOpType op_type = WriteOpType::INSERT,
                  const std::string& column = kValueColumn) {
    VLOG(4) << "Verifying data..." << std::endl;
    auto session = CreateSession();
    for (size_t i = 0; i != num_transactions; ++i) {
      VerifyRows(session, i, op_type, column);
    }
  }

  size_t CountTransactions() {
    size_t result = 0;
    for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
      auto* tablet_manager = cluster_->mini_tablet_server(i)->server()->tablet_manager();
      auto peers = tablet_manager->GetTabletPeers();
      for (const auto& peer : peers) {
        if (peer->consensus()->GetLeaderStatus() !=
                consensus::LeaderStatus::NOT_LEADER &&
            peer->tablet()->transaction_coordinator()) {
          result += peer->tablet()->transaction_coordinator()->test_count_transactions();
        }
      }
    }
    return result;
  }

  size_t CountIntents() {
    size_t result = 0;
    for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
      auto* tablet_manager = cluster_->mini_tablet_server(i)->server()->tablet_manager();
      auto peers = tablet_manager->GetTabletPeers();
      for (const auto &peer : peers) {
        auto participant = peer->tablet()->transaction_participant();
        if (participant) {
          result += participant->TEST_CountIntents();
        }
      }
    }
    return result;
  }

  void CheckNoRunningTransactions() {
    MonoTime deadline = MonoTime::Now() + 5s;
    bool has_bad = false;
    for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
      auto server = cluster_->mini_tablet_server(i)->server();
      auto tablets = server->tablet_manager()->GetTabletPeers();
      for (const auto& peer : tablets) {
        auto tablet_title = Format("Tablet: $0", peer->tablet()->tablet_id());
        auto participant = peer->tablet()->transaction_participant();
        if (participant) {
          auto status = Wait([participant] {
                return participant->TEST_GetNumRunningTransactions() == 0;
              },
              deadline,
              "Wait until no transactions are running");
          if (!status.ok()) {
            LOG(ERROR) << Format(
                "Server: $0, tablet: $1, transactions: $2",
                server->permanent_uuid(), peer->tablet()->tablet_id(),
                participant->TEST_GetNumRunningTransactions());
            has_bad = true;
          }
        }
      }
    }
    ASSERT_EQ(false, has_bad);
  }

  bool CheckAllTabletsRunning() {
    bool result = true;
    size_t count = 0;
    for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
      auto peers = cluster_->mini_tablet_server(i)->server()->tablet_manager()->GetTabletPeers();
      if (i == 0) {
        count = peers.size();
      } else if (count != peers.size()) {
        LOG(WARNING) << "Different number of tablets in tservers: "
                     << count << " vs " << peers.size() << " at " << i;
        result = false;
      }
      for (const auto& peer : peers) {
        auto status = peer->CheckRunning();
        if (!status.ok()) {
          LOG(WARNING) << Format("T $0 P $1 is not running: $2", peer->tablet_id(),
                                 peer->permanent_uuid(), status);
          result = false;
        }
      }
    }
    return result;
  }

  // We write data with first transaction then try to read it another one.
  // If commit is true, then first transaction is committed and second should be restarted.
  // Otherwise second transaction would see pending intents from first one and should not restart.
  void TestReadRestart(bool commit = true);

  void TestWriteConflicts(bool do_restarts);

  std::shared_ptr<server::SkewedClock> skewed_clock_{
      std::make_shared<server::SkewedClock>(WallClock())};
  server::ClockPtr clock_{new server::HybridClock(skewed_clock_)};
  boost::optional<TransactionManager> transaction_manager_;
  boost::optional<TransactionManager> transaction_manager2_;
};

TEST_F(QLTransactionTest, Simple) {
  WriteData();
  VerifyData();
  ASSERT_OK(cluster_->RestartSync());
  CheckNoRunningTransactions();
}

TEST_F(QLTransactionTest, LookupTabletFailure) {
  google::FlagSaver saver;
  FLAGS_master_inject_latency_on_transactional_tablet_lookups_ms =
      TransactionRpcTimeout().ToMilliseconds() + 500;

  auto txn = CreateTransaction();
  auto result = WriteRow(CreateSession(txn), 0 /* key */, 1 /* value */);

  ASSERT_TRUE(!result.ok() && result.status().IsTimedOut()) << "Result: " << result;
}

TEST_F(QLTransactionTest, ReadWithTimeInFuture) {
  WriteData();
  server::SkewedClockDeltaChanger delta_changer(100ms, skewed_clock_);
  for (size_t i = 0; i != 100; ++i) {
    auto transaction = CreateTransaction2();
    auto session = CreateSession(transaction);
    VerifyRows(session);
  }
  ASSERT_OK(cluster_->RestartSync());
  CheckNoRunningTransactions();
}

TEST_F(QLTransactionTest, WriteSameKey) {
  ASSERT_NO_FATALS(WriteDataWithRepetition());
  std::this_thread::sleep_for(1s); // Wait some time for intents to apply.
  ASSERT_NO_FATALS(VerifyData());
  ASSERT_OK(cluster_->RestartSync());
}

TEST_F(QLTransactionTest, WriteSameKeyWithIntents) {
  DisableApplyingIntents();

  ASSERT_NO_FATALS(WriteDataWithRepetition());
  ASSERT_NO_FATALS(VerifyData());
  ASSERT_OK(cluster_->RestartSync());
}

// Commit flags says whether we should commit write txn during this test.
void QLTransactionTest::TestReadRestart(bool commit) {
  SetAtomicFlag(250000ULL, &FLAGS_max_clock_skew_usec);

  {
    auto write_txn = CreateTransaction();
    WriteRows(CreateSession(write_txn));
    if (commit) {
      ASSERT_OK(write_txn->CommitFuture().get());
    }
    BOOST_SCOPE_EXIT(write_txn, commit) {
      if (!commit) {
        write_txn->Abort();
      }
    } BOOST_SCOPE_EXIT_END;

    server::SkewedClockDeltaChanger delta_changer(-100ms, skewed_clock_);

    auto txn1 = CreateTransaction2();
    BOOST_SCOPE_EXIT(txn1, commit) {
      if (!commit) {
        txn1->Abort();
      }
    } BOOST_SCOPE_EXIT_END;
    auto session = CreateSession(txn1);
    if (commit) {
      for (size_t r = 0; r != kNumRows; ++r) {
        auto row = SelectRow(session, KeyForTransactionAndIndex(0, r));
        ASSERT_NOK(row);
        ASSERT_EQ(ql::ErrorCode::RESTART_REQUIRED, ql::GetErrorCode(row.status()))
                      << "Bad row: " << row;
      }
      auto txn2 = txn1->CreateRestartedTransaction();
      BOOST_SCOPE_EXIT(txn2) {
        txn2->Abort();
      } BOOST_SCOPE_EXIT_END;
      session->SetTransaction(txn2);
      VerifyRows(session);
      VerifyData();
    } else {
      for (size_t r = 0; r != kNumRows; ++r) {
        auto row = SelectRow(session, KeyForTransactionAndIndex(0, r));
        ASSERT_TRUE(!row.ok() && row.status().IsNotFound()) << "Bad row: " << row;
      }
    }
  }

  ASSERT_OK(cluster_->RestartSync());
}

TEST_F(QLTransactionTest, ReadRestart) {
  TestReadRestart();
  CheckNoRunningTransactions();
}

TEST_F(QLTransactionTest, ReadRestartWithIntents) {
  DisableApplyingIntents();
  TestReadRestart();
}

TEST_F(QLTransactionTest, ReadRestartWithPendingIntents) {
  FLAGS_transaction_allow_rerequest_status_in_tests = false;
  DisableApplyingIntents();
  TestReadRestart(false /* commit */);
}

// Non transactional restart happens in server, so we just checking that we read correct values.
// Skewed clocks are used because there could be case when applied intents or commit transaction
// has time greater than max safetime to read, that causes restart.
TEST_F(QLTransactionTest, ReadRestartNonTransactional) {
  const auto kClockSkew = 500ms;

  SetAtomicFlag(1000000ULL, &FLAGS_max_clock_skew_usec);
  DisableTransactionTimeout();

  auto delta_changers = SkewClocks(cluster_.get(), kClockSkew);
  constexpr size_t kTotalTransactions = 10;

  for (size_t i = 0; i != kTotalTransactions; ++i) {
    SCOPED_TRACE(Format("Transaction $0", i));
    auto txn = CreateTransaction();
    WriteRows(CreateSession(txn), i);
    ASSERT_OK(txn->CommitFuture().get());
    ASSERT_NO_FATALS(VerifyRows(CreateSession(), i));

    // We propagate hybrid time, so when commit and read finishes, all servers has about the same
    // physical component. We are waiting double skew, until time on servers became skewed again.
    std::this_thread::sleep_for(kClockSkew * 2);
  }

  cluster_->Shutdown(); // Need to shutdown cluster before resetting clock back.
  cluster_.reset();
}

TEST_F(QLTransactionTest, WriteRestart) {
  SetAtomicFlag(250000ULL, &FLAGS_max_clock_skew_usec);

  const std::string kExtraColumn = "v2";
  std::unique_ptr<YBTableAlterer> table_alterer(client_->NewTableAlterer(kTableName));
  table_alterer->AddColumn(kExtraColumn)->Type(DataType::INT32);
  ASSERT_OK(table_alterer->Alter());

  ASSERT_OK(table_.Open(kTableName, client_.get())); // Reopen to update schema version.

  WriteData();

  server::SkewedClockDeltaChanger delta_changer(-100ms, skewed_clock_);
  auto txn1 = CreateTransaction2();
  YBTransactionPtr txn2;
  auto session = CreateSession(txn1);
  for (bool retry : {false, true}) {
    for (size_t r = 0; r != kNumRows; ++r) {
      const auto op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_UPDATE);
      auto* const req = op->mutable_request();
      auto key = KeyForTransactionAndIndex(0, r);
      auto old_value = ValueForTransactionAndIndex(0, r, WriteOpType::INSERT);
      auto value = ValueForTransactionAndIndex(0, r, WriteOpType::UPDATE);
      QLAddInt32HashValue(req, key);
      table_.AddInt32ColumnValue(req, kExtraColumn, value);
      auto cond = req->mutable_where_expr()->mutable_condition();
      table_.SetInt32Condition(cond, kValueColumn, QLOperator::QL_OP_EQUAL, old_value);
      req->mutable_column_refs()->add_ids(table_.ColumnId(kValueColumn));
      LOG(INFO) << "Updating value";
      auto status = session->ApplyAndFlush(op);
      ASSERT_OK(status);
      if (!retry) {
        ASSERT_EQ(QLResponsePB::YQL_STATUS_RESTART_REQUIRED_ERROR, op->response().status());
      } else {
        ASSERT_EQ(QLResponsePB::YQL_STATUS_OK, op->response().status());
      }
    }
    if (!retry) {
      txn2 = txn1->CreateRestartedTransaction();
      session->SetTransaction(txn2);
    }
  }
  txn2->CommitFuture().wait();
  VerifyData();
  VerifyData(1, WriteOpType::UPDATE, kExtraColumn);

  ASSERT_OK(cluster_->RestartSync());
  CheckNoRunningTransactions();
}

// Commit flags says whether we should commit write txn during this test.
TEST_F(QLTransactionTest, WriteAfterReadRestart) {
  const auto kClockDelta = 100ms;
  SetAtomicFlag(250000ULL, &FLAGS_max_clock_skew_usec);

  auto write_txn = CreateTransaction();
  WriteRows(CreateSession(write_txn));
  ASSERT_OK(write_txn->CommitFuture().get());

  server::SkewedClockDeltaChanger delta_changer(-kClockDelta, skewed_clock_);

  auto txn1 = CreateTransaction2();
  auto session = CreateSession(txn1);
  for (size_t r = 0; r != kNumRows; ++r) {
    auto row = SelectRow(session, KeyForTransactionAndIndex(0, r));
    ASSERT_NOK(row);
    ASSERT_EQ(ql::ErrorCode::RESTART_REQUIRED, ql::GetErrorCode(row.status()))
                  << "Bad row: " << row;
  }
  {
    // To reset clock back.
    auto temp_delta_changed = std::move(delta_changer);
  }
  auto txn2 = txn1->CreateRestartedTransaction();
  session->SetTransaction(txn2);
  VerifyRows(session);
  WriteRows(session, 0, WriteOpType::UPDATE);
  ASSERT_OK(txn2->CommitFuture().get());

  VerifyData(1, WriteOpType::UPDATE);
}

TEST_F(QLTransactionTest, Child) {
  auto txn = CreateTransaction();
  TransactionManager manager2(client_, clock_, client::LocalTabletFilter());
  auto data_pb = txn->PrepareChildFuture().get();
  ASSERT_OK(data_pb);
  auto data = ChildTransactionData::FromPB(*data_pb);
  ASSERT_OK(data);
  auto txn2 = std::make_shared<YBTransaction>(&manager2, std::move(*data));

  WriteRows(CreateSession(txn2), 0);
  auto result = txn2->FinishChild();
  ASSERT_OK(result);
  ASSERT_OK(txn->ApplyChildResult(*result));

  ASSERT_OK(txn->CommitFuture().get());

  ASSERT_NO_FATALS(VerifyData());
  ASSERT_OK(cluster_->RestartSync());
  CheckNoRunningTransactions();
}

TEST_F(QLTransactionTest, ChildReadRestart) {
  SetAtomicFlag(250000ULL, &FLAGS_max_clock_skew_usec);

  {
    auto write_txn = CreateTransaction();
    WriteRows(CreateSession(write_txn));
    ASSERT_OK(write_txn->CommitFuture().get());
  }

  server::SkewedClockDeltaChanger delta_changer(-100ms, skewed_clock_);
  auto parent_txn = CreateTransaction2();

  auto data_pb = parent_txn->PrepareChildFuture().get();
  ASSERT_OK(data_pb);
  auto data = ChildTransactionData::FromPB(*data_pb);
  ASSERT_OK(data);

  server::ClockPtr clock3(new server::HybridClock(skewed_clock_));
  ASSERT_OK(clock3->Init());
  TransactionManager manager3(client_, clock3, client::LocalTabletFilter());
  auto child_txn = std::make_shared<YBTransaction>(&manager3, std::move(*data));

  auto session = CreateSession(child_txn);
  for (size_t r = 0; r != kNumRows; ++r) {
    auto row = SelectRow(session, KeyForTransactionAndIndex(0, r));
    ASSERT_NOK(row);
    ASSERT_EQ(ql::ErrorCode::RESTART_REQUIRED, ql::GetErrorCode(row.status()))
                  << "Bad row: " << row;
  }

  auto result = child_txn->FinishChild();
  ASSERT_OK(result);
  ASSERT_OK(parent_txn->ApplyChildResult(*result));

  auto master2_txn = parent_txn->CreateRestartedTransaction();
  session->SetTransaction(master2_txn);
  for (size_t r = 0; r != kNumRows; ++r) {
    auto row = SelectRow(session, KeyForTransactionAndIndex(0, r));
    ASSERT_OK(row);
    ASSERT_EQ(ValueForTransactionAndIndex(0, r, WriteOpType::INSERT), *row);
  }
  ASSERT_NO_FATALS(VerifyData());

  ASSERT_OK(cluster_->RestartSync());
  CheckNoRunningTransactions();
}

TEST_F(QLTransactionTest, InsertUpdate) {
  DisableApplyingIntents();
  WriteData(); // Add data
  WriteData(); // Update data
  VerifyData();
  ASSERT_OK(cluster_->RestartSync());
}

TEST_F(QLTransactionTest, Cleanup) {
  WriteData();
  VerifyData();

  // Wait transaction apply. Otherwise count could be non zero.
  ASSERT_OK(WaitFor(
      [this] { return CountTransactions() == 0; }, kTransactionApplyTime, "Transactions cleaned"));
  VerifyData();
  ASSERT_OK(cluster_->RestartSync());
  CheckNoRunningTransactions();
}

TEST_F(QLTransactionTest, Heartbeat) {
  auto txn = CreateTransaction();
  auto session = CreateSession(txn);
  WriteRows(session);
  std::this_thread::sleep_for(GetTransactionTimeout() * 2);
  CountDownLatch latch(1);
  txn->Commit([&latch](const Status& status) {
    EXPECT_OK(status);
    latch.CountDown();
  });
  latch.Wait();
  VerifyData();
  CheckNoRunningTransactions();
}

TEST_F(QLTransactionTest, Expire) {
  SetDisableHeartbeatInTests(true);
  auto txn = CreateTransaction();
  auto session = CreateSession(txn);
  WriteRows(session);
  std::this_thread::sleep_for(GetTransactionTimeout() * 2);
  CountDownLatch latch(1);
  txn->Commit([&latch](const Status& status) {
    EXPECT_TRUE(status.IsExpired()) << "Bad status: " << status.ToString();
    latch.CountDown();
  });
  latch.Wait();
  std::this_thread::sleep_for(std::chrono::microseconds(FLAGS_transaction_heartbeat_usec * 2));
  ASSERT_OK(cluster_->CleanTabletLogs());
  ASSERT_EQ(0, CountTransactions());
}

TEST_F(QLTransactionTest, PreserveLogs) {
  SetDisableHeartbeatInTests(true);
  DisableTransactionTimeout();
  std::vector<std::shared_ptr<YBTransaction>> transactions;
  constexpr size_t kTransactions = 20;
  for (size_t i = 0; i != kTransactions; ++i) {
    auto txn = CreateTransaction();
    auto session = CreateSession(txn);
    WriteRows(session, i);
    transactions.push_back(std::move(txn));
    std::this_thread::sleep_for(100ms);
  }
  LOG(INFO) << "Request clean";
  ASSERT_OK(cluster_->CleanTabletLogs());
  ASSERT_OK(cluster_->RestartSync());
  CountDownLatch latch(kTransactions);
  for (auto& transaction : transactions) {
    transaction->Commit([&latch](const Status& status) {
      EXPECT_OK(status);
      latch.CountDown();
    });
  }
  latch.Wait();
  VerifyData(kTransactions);
  CheckNoRunningTransactions();
}

TEST_F(QLTransactionTest, ResendApplying) {
  DisableApplyingIntents();
  WriteData();
  std::this_thread::sleep_for(5s); // Transaction should not be applied here.
  ASSERT_NE(0, CountTransactions());

  SetIgnoreApplyingProbability(0.0);

  ASSERT_OK(WaitFor(
      [this] { return CountTransactions() == 0; }, kTransactionApplyTime, "Transactions cleaned"));
  VerifyData();
  ASSERT_OK(cluster_->RestartSync());
  CheckNoRunningTransactions();
}

TEST_F(QLTransactionTest, ConflictResolution) {
  constexpr size_t kTotalTransactions = 5;
  constexpr size_t kNumRows = 10;
  std::vector<YBTransactionPtr> transactions;
  std::vector<YBSessionPtr> sessions;

  CountDownLatch latch(kTotalTransactions);
  for (size_t i = 0; i != kTotalTransactions; ++i) {
    transactions.push_back(CreateTransaction());
    auto session = CreateSession(transactions.back());
    sessions.push_back(session);
    for (size_t r = 0; r != kNumRows; ++r) {
      ASSERT_OK(WriteRow(sessions.back(), r, i, WriteOpType::INSERT, Flush::kFalse));
    }
    session->FlushAsync([&latch](const Status& status) { latch.CountDown(); });
  }
  latch.Wait();

  latch.Reset(transactions.size());
  std::atomic<size_t> successes(0);
  std::atomic<size_t> failures(0);

  for (auto& transaction : transactions) {
    transaction->Commit([&latch, &successes, &failures](const Status& status) {
      if (status.ok()) {
        successes.fetch_add(1, std::memory_order_release);
      } else {
        failures.fetch_add(1, std::memory_order_release);
      }
      latch.CountDown(1);
    });
  }

  latch.Wait();
  LOG(INFO) << "Committed, successes: " << successes.load() << ", failures: " << failures.load();

  ASSERT_GE(successes.load(std::memory_order_acquire), 1);

  auto session = CreateSession();
  std::vector<int32_t> values;
  for (size_t r = 0; r != kNumRows; ++r) {
    auto row = SelectRow(session, r);
    ASSERT_OK(row);
    values.push_back(*row);
  }
  for (const auto& value : values) {
    ASSERT_EQ(values.front(), value) << "Values: " << yb::ToString(values);
  }
}

TEST_F(QLTransactionTest, SimpleWriteConflict) {
  auto transaction = CreateTransaction();
  WriteRows(CreateSession(transaction));
  WriteRows(CreateSession());

  ASSERT_NOK(transaction->CommitFuture().get());
}

void QLTransactionTest::TestWriteConflicts(bool do_restarts) {
  struct ActiveTransaction {
    YBTransactionPtr transaction;
    YBSessionPtr session;
    std::future<Status> flush_future;
    std::future<Status> commit_future;
  };

  constexpr size_t kActiveTransactions = 50;
  constexpr auto kTestTime = 60s;
  constexpr int kTotalKeys = 5;
  std::vector<ActiveTransaction> active_transactions;

  auto stop = std::chrono::steady_clock::now() + kTestTime;

  std::thread restart_thread;

  if (do_restarts) {
    restart_thread = std::thread([this, stop] {
        int it = 0;
        while (std::chrono::steady_clock::now() < stop) {
          std::this_thread::sleep_for(5s);
          ASSERT_OK(cluster_->mini_tablet_server(++it % cluster_->num_tablet_servers())->Restart());
        }
    });
  }

  int value = 0;
  size_t tries = 0;
  size_t written = 0;
  size_t flushed = 0;
  for (;;) {
    auto expired = std::chrono::steady_clock::now() >= stop;
    if (expired) {
      if (active_transactions.empty()) {
        break;
      }
      LOG(INFO) << "Time expired, remaining transactions: " << active_transactions.size();
      for (const auto& txn : active_transactions) {
        LOG(INFO) << "TXN: " << txn.transaction->ToString() << ", "
                  << (!txn.commit_future.valid() ? "Flushing" : "Committing");
      }
    }
    while (!expired && active_transactions.size() < kActiveTransactions) {
      auto key = RandomUniformInt(1, kTotalKeys);
      ActiveTransaction active_txn;
      active_txn.transaction = CreateTransaction();
      active_txn.session = CreateSession(active_txn.transaction);
      const auto op = table_.NewInsertOp();
      auto* const req = op->mutable_request();
      QLAddInt32HashValue(req, key);
      table_.AddInt32ColumnValue(req, kValueColumn, ++value);
      ASSERT_OK(active_txn.session->Apply(op));
      active_txn.flush_future = active_txn.session->FlushFuture();

      ++tries;
      active_transactions.push_back(std::move(active_txn));
    }

    auto w = active_transactions.begin();
    for (auto i = active_transactions.begin(); i != active_transactions.end(); ++i) {
      if (!i->commit_future.valid()) {
        if (i->flush_future.wait_for(0s) == std::future_status::ready) {
          auto flush_status = i->flush_future.get();
          if (!flush_status.ok()) {
            LOG(INFO) << "Flush failed: " << flush_status;
            continue;
          }
          ++flushed;
          i->commit_future = i->transaction->CommitFuture();
        }
      } else if (i->commit_future.wait_for(0s) == std::future_status::ready) {
        auto commit_status = i->commit_future.get();
        if (!commit_status.ok()) {
          LOG(INFO) << "Commit failed: " << commit_status;
          continue;
        }
        ++written;
        continue;
      }

      if (w != i) {
        *w = std::move(*i);
      }
      ++w;
    }
    active_transactions.erase(w, active_transactions.end());

    std::this_thread::sleep_for(expired ? 1s : 100ms);
  }

  if (do_restarts) {
    restart_thread.join();
  }

  ASSERT_GE(written, kTotalKeys);
  ASSERT_GE(flushed, written);
  ASSERT_GE(flushed, kActiveTransactions);
  ASSERT_GE(tries, flushed);
}

class WriteConflictsTest : public QLTransactionTest {
 protected:
  uint64_t log_segment_size_bytes() const override {
    return 0;
  }
};

TEST_F_EX(QLTransactionTest, WriteConflicts, WriteConflictsTest) {
  TestWriteConflicts(false /* do_restarts */);
}

TEST_F_EX(QLTransactionTest, WriteConflictsWithRestarts, WriteConflictsTest) {
  TestWriteConflicts(true /* do_restarts */);
}

TEST_F(QLTransactionTest, ResolveIntentsWriteReadUpdateRead) {
  DisableApplyingIntents();

  WriteData();
  VerifyData();

  WriteData(WriteOpType::UPDATE);
  VerifyData(1, WriteOpType::UPDATE);

  ASSERT_OK(cluster_->RestartSync());
}

TEST_F(QLTransactionTest, ResolveIntentsWriteReadWithinTransactionAndRollback) {
  SetAtomicFlag(0ULL, &FLAGS_max_clock_skew_usec); // To avoid read restart in this test.
  DisableApplyingIntents();

  // Write { 1 -> 1, 2 -> 2 }.
  {
    auto session = CreateSession();
    ASSERT_OK(WriteRow(session, 1, 1));
    ASSERT_OK(WriteRow(session, 2, 2));
  }

  {
    // Start T1.
    auto txn = CreateTransaction();
    auto session = CreateSession(txn);

    // T1: Update { 1 -> 11, 2 -> 12 }.
    ASSERT_OK(UpdateRow(session, 1, 11));
    ASSERT_OK(UpdateRow(session, 2, 12));

    // T1: Should read { 1 -> 11, 2 -> 12 }.
    VERIFY_ROW(session, 1, 11);
    VERIFY_ROW(session, 2, 12);

    txn->Abort();
  }

  ASSERT_OK(WaitFor(
      [this] { return CountTransactions() == 0; }, kTransactionApplyTime, "Transactions cleaned"));

  // Should read { 1 -> 1, 2 -> 2 }, since T1 has been aborted.
  {
    auto session = CreateSession();
    VERIFY_ROW(session, 1, 1);
    VERIFY_ROW(session, 2, 2);
  }

  ASSERT_EQ(CountIntents(), 0);

  ASSERT_OK(cluster_->RestartSync());
}

TEST_F(QLTransactionTest, CheckCompactionAbortCleanup) {
  SetAtomicFlag(0ULL, &FLAGS_max_clock_skew_usec); // To avoid read restart in this test.
  FLAGS_transaction_disable_proactive_cleanup_in_tests = true;
  FLAGS_aborted_intent_cleanup_ms = 1000; // 1 sec

  // Write { 1 -> 1, 2 -> 2 }.
  {
    auto session = CreateSession();
    ASSERT_OK(WriteRow(session, 1, 1));
    ASSERT_OK(WriteRow(session, 2, 2));
  }

  {
    // Start T1.
    auto txn = CreateTransaction();
    auto session = CreateSession(txn);

    // T1: Update { 1 -> 11, 2 -> 12 }.
    ASSERT_OK(UpdateRow(session, 1, 11));
    ASSERT_OK(UpdateRow(session, 2, 12));

    // T1: Should read { 1 -> 11, 2 -> 12 }.
    VERIFY_ROW(session, 1, 11);
    VERIFY_ROW(session, 2, 12);

    txn->Abort();
  }

  ASSERT_OK(WaitFor(
      [this] { return CountTransactions() == 0; }, kTransactionApplyTime, "Transactions cleaned"));

  std::this_thread::sleep_for(std::chrono::microseconds(FLAGS_aborted_intent_cleanup_ms));
  tserver::TSTabletManager::TabletPeers peers;
  cluster_->mini_tablet_server(0)->server()->tablet_manager()->GetTabletPeers(&peers);
  for (std::shared_ptr<tablet::TabletPeer>  peer : peers) {
    peer->tablet()->ForceRocksDBCompactInTest();
  }

  // Should read { 1 -> 1, 2 -> 2 }, since T1 has been aborted.
  {
    auto session = CreateSession();
    VERIFY_ROW(session, 1, 1);
    VERIFY_ROW(session, 2, 2);
  }

  ASSERT_EQ(CountIntents(), 0);

  ASSERT_OK(cluster_->RestartSync());
}

TEST_F(QLTransactionTest, ResolveIntentsWriteReadBeforeAndAfterCommit) {
  SetAtomicFlag(0ULL, &FLAGS_max_clock_skew_usec); // To avoid read restart in this test.
  DisableApplyingIntents();

  // Write { 1 -> 1, 2 -> 2 }.
  {
    auto session = CreateSession();
    ASSERT_OK(WriteRow(session, 1, 1));
    ASSERT_OK(WriteRow(session, 2, 2));
  }

  // Start T1.
  auto txn1 = CreateTransaction();
  auto session1 = CreateSession(txn1);

  // T1: Update { 1 -> 11, 2 -> 12 }.
  ASSERT_OK(UpdateRow(session1, 1, 11));
  ASSERT_OK(UpdateRow(session1, 2, 12));

  // Start T2.
  auto txn2 = CreateTransaction();
  auto session2 = CreateSession(txn2);

  // T2: Should read { 1 -> 1, 2 -> 2 }.
  VERIFY_ROW(session2, 1, 1);
  VERIFY_ROW(session2, 2, 2);

  // T1: Commit
  CommitAndResetSync(&txn1);

  // T2: Should still read { 1 -> 1, 2 -> 2 }, because it should read at the time of it's start.
  VERIFY_ROW(session2, 1, 1);
  VERIFY_ROW(session2, 2, 2);

  // Simple read should get { 1 -> 11, 2 -> 12 }, since T1 has been already committed.
  {
    auto session = CreateSession();
    VERIFY_ROW(session, 1, 11);
    VERIFY_ROW(session, 2, 12);
  }

  CommitAndResetSync(&txn2);

  ASSERT_OK(cluster_->RestartSync());
}

TEST_F(QLTransactionTest, ResolveIntentsCheckConsistency) {
  SetAtomicFlag(0ULL, &FLAGS_max_clock_skew_usec); // To avoid read restart in this test.
  DisableApplyingIntents();

  // Write { 1 -> 1, 2 -> 2 }.
  {
    auto session = CreateSession();
    ASSERT_OK(WriteRow(session, 1, 1));
    ASSERT_OK(WriteRow(session, 2, 2));
  }

  // Start T1.
  auto txn1 = CreateTransaction();

  // T1: Update { 1 -> 11, 2 -> 12 }.
  {
    auto session = CreateSession(txn1);
    ASSERT_OK(UpdateRow(session, 1, 11));
    ASSERT_OK(UpdateRow(session, 2, 12));
  }

  // T1: Request commit.
  CountDownLatch commit_latch(1);
  txn1->Commit([&commit_latch](const Status& status) {
    ASSERT_OK(status);
    commit_latch.CountDown(1);
  });

  // Start T2.
  auto txn2 = CreateTransaction();

  // T2: Should read { 1 -> 1, 2 -> 2 } even in case T1 is committed between reading k1 and k2.
  {
    auto session = CreateSession(txn2);
    VERIFY_ROW(session, 1, 1);
    commit_latch.Wait();
    VERIFY_ROW(session, 2, 2);
  }

  // Simple read should get { 1 -> 11, 2 -> 12 }, since T1 has been already committed.
  {
    auto session = CreateSession();
    VERIFY_ROW(session, 1, 11);
    VERIFY_ROW(session, 2, 12);
  }

  CommitAndResetSync(&txn2);

  ASSERT_OK(cluster_->RestartSync());
}

// This test launches write thread, that writes increasing value to key using transaction.
// Then it launches multiple read threads, each of them tries to read this key and
// verifies that its value is at least the same like it was written before read was started.
//
// It is don't for multiple keys sequentially. So those keys are located on different tablets
// and tablet servers, and we test different cases of clock skew.
TEST_F(QLTransactionTest, CorrectStatusRequestBatching) {
  const auto kClockSkew = 100ms;
  constexpr auto kMinWrites = RegularBuildVsSanitizers(25, 1);
  constexpr auto kMinReads = 10;
  constexpr size_t kConcurrentReads = RegularBuildVsSanitizers<size_t>(20, 5);

  FLAGS_transaction_delay_status_reply_usec_in_tests = 200000;
  FLAGS_log_segment_size_bytes = 0;
  SetAtomicFlag(std::chrono::microseconds(kClockSkew).count() * 3, &FLAGS_max_clock_skew_usec);

  auto delta_changers = SkewClocks(cluster_.get(), kClockSkew);

  for (int32_t key = 0; key != 10; ++key) {
    std::atomic<bool> stop(false);
    std::atomic<int32_t> value(0);

    std::thread write_thread([this, key, &stop, &value] {
      auto session = CreateSession();
      while (!stop) {
        auto txn = CreateTransaction();
        session->SetTransaction(txn);
        auto write_result = WriteRow(session, key, value + 1);
        if (write_result.ok()) {
          auto status = txn->CommitFuture().get();
          if (status.ok()) {
            ++value;
          }
        }
      }
    });

    std::vector<std::thread> read_threads;
    std::array<std::atomic<size_t>, kConcurrentReads> reads;
    for (auto& read : reads) {
      read.store(0);
    }

    for (size_t i = 0; i != kConcurrentReads; ++i) {
      read_threads.emplace_back([this, key, &stop, &value, &read = reads[i]] {
        auto session = CreateSession();
        bool ok = false;
        BOOST_SCOPE_EXIT(&ok, &stop) {
          if (!ok) {
            stop = true;
          }
        } BOOST_SCOPE_EXIT_END;
        while (!stop) {
          auto value_before_start = value.load();
          YBqlReadOpPtr op = ReadRow(session, key);
          ASSERT_OK(session->Flush());
          ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK)
                        << op->response().ShortDebugString();
          auto rowblock = yb::ql::RowsResult(op.get()).GetRowBlock();
          int32_t current_value;
          if (rowblock->row_count() == 0) {
            current_value = 0;
          } else {
            current_value = rowblock->row(0).column(0).int32_value();
          }
          ASSERT_GE(current_value, value_before_start);
          ++read;
        }
        ok = true;
      });
    }

    auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!stop && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(100ms);
    }

    // Already failed
    bool failed = stop.exchange(true);
    write_thread.join();

    for (auto& thread : read_threads) {
      thread.join();
    }

    if (failed) {
      break;
    }

    LOG(INFO) << "Writes: " << value.load() << ", reads: " << yb::ToString(reads);

    EXPECT_GE(value.load(), kMinWrites);
    for (auto& read : reads) {
      EXPECT_GE(read.load(), kMinReads);
    }
  }

  cluster_->Shutdown(); // Need to shutdown cluster before resetting clock back.
  cluster_.reset();
}

struct TransactionState {
  YBTransactionPtr transaction;
  std::shared_future<TransactionMetadata> metadata_future;
  std::future<Status> commit_future;
  std::future<Result<tserver::GetTransactionStatusResponsePB>> status_future;
  TransactionMetadata metadata;
  HybridTime status_time = HybridTime::kMin;
  TransactionStatus last_status = TransactionStatus::PENDING;

  void CheckStatus() {
    ASSERT_TRUE(status_future.valid());
    ASSERT_EQ(status_future.wait_for(NonTsanVsTsan(3s, 10s)), std::future_status::ready);
    auto resp = status_future.get();
    ASSERT_OK(resp);

    if (resp->status() == TransactionStatus::ABORTED) {
      ASSERT_TRUE(commit_future.valid());
      transaction = nullptr;
      return;
    }

    auto new_time = HybridTime(resp->status_hybrid_time());
    if (last_status == TransactionStatus::PENDING) {
      if (resp->status() == TransactionStatus::PENDING) {
        ASSERT_GE(new_time, status_time);
      } else {
        ASSERT_EQ(TransactionStatus::COMMITTED, resp->status());
        ASSERT_GT(new_time, status_time);
      }
    } else {
      ASSERT_EQ(last_status, TransactionStatus::COMMITTED);
      ASSERT_EQ(resp->status(), TransactionStatus::COMMITTED)
          << "Bad transaction status: " << TransactionStatus_Name(resp->status());
      ASSERT_EQ(status_time, new_time);
    }
    status_time = new_time;
    last_status = resp->status();
  }
};

// Test transaction status evolution.
// The following should happen:
// If both previous and new transaction state are PENDING, then the new time of status is >= the
// old time of status.
// Previous - PENDING, new - COMMITTED, new_time > old_time.
// Previous - COMMITTED, new - COMMITTED, new_time == old_time.
// All other cases are invalid.
TEST_F(QLTransactionTest, StatusEvolution) {
  // We don't care about exact probability of create/commit operations.
  // Just create rate should be higher than commit one.
  const int kTransactionCreateChance = 10;
  const int kTransactionCommitChance = 20;
  size_t transactions_to_create = 10;
  size_t active_transactions = 0;
  std::vector<TransactionState> states;
  rpc::Rpcs rpcs;
  states.reserve(transactions_to_create);

  while (transactions_to_create || active_transactions) {
    if (transactions_to_create &&
        (!active_transactions || RandomWithChance(kTransactionCreateChance))) {
      LOG(INFO) << "Create transaction";
      auto txn = CreateTransaction();
      {
        auto session = CreateSession(txn);
        // Insert using different keys to avoid conflicts.
        ASSERT_OK(WriteRow(session, states.size(), states.size()));
      }
      states.push_back({ txn, txn->TEST_GetMetadata() });
      ++active_transactions;
      --transactions_to_create;
    }
    if (active_transactions && RandomWithChance(kTransactionCommitChance)) {
      LOG(INFO) << "Destroy transaction";
      size_t idx = RandomUniformInt<size_t>(1, active_transactions);
      for (auto& state : states) {
        if (!state.transaction) {
          continue;
        }
        if (!--idx) {
          state.commit_future = state.transaction->CommitFuture();
          break;
        }
      }
    }

    for (auto& state : states) {
      if (!state.transaction) {
        continue;
      }
      if (state.metadata.isolation == IsolationLevel::NON_TRANSACTIONAL) {
        if (state.metadata_future.wait_for(0s) != std::future_status::ready) {
          continue;
        }
        state.metadata = state.metadata_future.get();
      }
      tserver::GetTransactionStatusRequestPB req;
      req.set_tablet_id(state.metadata.status_tablet);
      req.set_transaction_id(state.metadata.transaction_id.data,
                             state.metadata.transaction_id.size());
      state.status_future = rpc::WrapRpcFuture<tserver::GetTransactionStatusResponsePB>(
          GetTransactionStatus, &rpcs)(
              TransactionRpcDeadline(), nullptr /* tablet */, client_.get(), &req);
    }
    for (auto& state : states) {
      if (!state.transaction) {
        continue;
      }
      state.CheckStatus();
      if (!state.transaction) {
        --active_transactions;
      }
    }
  }

  for (auto& state : states) {
    ASSERT_EQ(state.commit_future.wait_for(NonTsanVsTsan(3s, 15s)), std::future_status::ready);
  }
}

// Writing multiple keys concurrently, each key is increasing by 1 at each step.
// At the same time concurrently execute several transactions that read all those keys.
// Suppose two transactions have read values t1_i and t2_i respectively.
// And t1_j > t2_j for some j, then we expect that t1_i >= t2_i for all i.
//
// Suppose we have 2 transactions, both reading k1 (from tablet1) and k2 (from tablet2).
// ht1 - read time of first transaction, and ht2 - read time of second transaction.
// Suppose ht1 <= ht2 for simplicity.
// Old value of k1 is v1before, and after ht_k1 it has v1after.
// Old value of k2 is v2before, and after ht_k2 it has v2after.
// ht_k1 <= ht1, ht_k2 <= ht1.
//
// Suppose following sequence of read requests:
// 1) The read request for the first transaction arrives at tablet1 when it has safe read
//    time < ht1. But it is already replicating k1 (with ht_k1). Then it would read v1before for k1.
// 2) The read request for the second transaction arrives at tablet2 when it has safe read
//    time < ht2. But it is already replicating k2 (with ht_k2). So it reads v2before for k2.
// 3) The remaining read request requests arrive after the appropriate operations have replicated.
//    So we get v2after in the first transaction and v1after for the second.
// The read result for the first transaction (v1before, v2after), for the second is is
// (v1after, v2before).
//
// Such read is inconsistent.
//
// This test addresses this issue.
TEST_F(QLTransactionTest, WaitRead) {
  constexpr size_t kWriteThreads = 10;
  constexpr size_t kCycles = 100;
  constexpr size_t kConcurrentReads = 4;

  SetAtomicFlag(0ULL, &FLAGS_max_clock_skew_usec); // To avoid read restart in this test.

  std::atomic<bool> stop(false);
  std::vector<std::thread> threads;

  for (size_t i = 0; i != kWriteThreads; ++i) {
    threads.emplace_back([this, i, &stop] {
      auto session = CreateSession();
      int32_t value = 0;
      while (!stop) {
        ASSERT_OK(WriteRow(session, i, ++value));
      }
    });
  }

  CountDownLatch latch(kConcurrentReads);

  std::vector<std::vector<YBqlReadOpPtr>> reads(kConcurrentReads);
  std::vector<std::shared_future<Status>> futures(kConcurrentReads);
  // values[i] contains values read by i-th transaction.
  std::vector<std::vector<int32_t>> values(kConcurrentReads);

  for (size_t i = 0; i != kCycles; ++i) {
    latch.Reset(kConcurrentReads);
    for (size_t j = 0; j != kConcurrentReads; ++j) {
      values[j].clear();
      auto session = CreateSession(CreateTransaction());
      for (size_t key = 0; key != kWriteThreads; ++key) {
        reads[j].push_back(ReadRow(session, key));
      }
      session->FlushAsync([&latch](const Status& status) {
        ASSERT_OK(status);
        latch.CountDown();
      });
    }
    latch.Wait();
    for (size_t j = 0; j != kConcurrentReads; ++j) {
      values[j].clear();
      for (auto& op : reads[j]) {
        ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK)
            << op->response().ShortDebugString();
        auto rowblock = yb::ql::RowsResult(op.get()).GetRowBlock();
        if (rowblock->row_count() == 1) {
          values[j].push_back(rowblock->row(0).column(0).int32_value());
        } else {
          values[j].push_back(0);
        }
      }
    }
    std::sort(values.begin(), values.end());
    for (size_t j = 1; j != kConcurrentReads; ++j) {
      for (size_t k = 0; k != values[j].size(); ++k) {
        ASSERT_GE(values[j][k], values[j - 1][k]);
      }
    }
  }

  stop = true;
  for (auto& thread : threads) {
    thread.join();
  }
}

TEST_F(QLTransactionTest, InsertDelete) {
  DisableApplyingIntents();

  auto txn = CreateTransaction();
  auto session = CreateSession(txn);
  ASSERT_OK(WriteRow(session, 1 /* key */, 10 /* value */, WriteOpType::INSERT));
  ASSERT_OK(DeleteRow(session, 1 /* key */));
  ASSERT_OK(txn->CommitFuture().get());

  session = CreateSession();
  auto row = SelectRow(session, 1 /* key */);
  ASSERT_FALSE(row.ok()) << "Row: " << row;
}

TEST_F(QLTransactionTest, InsertDeleteWithClusterRestart) {
  DisableApplyingIntents();
  DisableTransactionTimeout();
  constexpr int kKeys = 100;

  for (int i = 0; i != kKeys; ++i) {
    ASSERT_OK(WriteRow(CreateSession(), i /* key */, i * 2 /* value */, WriteOpType::INSERT));
  }

  auto txn = CreateTransaction();
  auto session = CreateSession(txn);
  for (int i = 0; i != kKeys; ++i) {
    SCOPED_TRACE(Format("Key: $0", i));
    ASSERT_OK(WriteRow(session, i /* key */, i * 3 /* value */, WriteOpType::UPDATE));
  }

  std::this_thread::sleep_for(1s); // Wait some time for intents to populate.
  ASSERT_OK(cluster_->RestartSync());

  for (int i = 0; i != kKeys; ++i) {
    SCOPED_TRACE(Format("Key: $0", i));
    ASSERT_OK(DeleteRow(session, i /* key */));
  }
  ASSERT_OK(txn->CommitFuture().get());

  session = CreateSession();
  for (int i = 0; i != kKeys; ++i) {
    SCOPED_TRACE(Format("Key: $0", i));
    auto row = SelectRow(session, 1 /* key */);
    ASSERT_FALSE(row.ok()) << "Row: " << row;
  }
}

TEST_F(QLTransactionTest, ChangeLeader) {
  constexpr size_t kThreads = 2;
  constexpr auto kTestTime = 5s;

  DisableTransactionTimeout();

  std::vector<std::thread> threads;
  std::atomic<bool> stopped{false};
  for (size_t i = 0; i != kThreads; ++i) {
    threads.emplace_back([this, i, &stopped] {
      size_t idx = i;
      while (!stopped) {
        WriteData(WriteOpType::INSERT, idx);
        idx += kThreads;
      }
    });
  }

  auto test_finish = std::chrono::steady_clock::now() + kTestTime;
  while (std::chrono::steady_clock::now() < test_finish) {
    for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
      std::vector<tablet::TabletPeerPtr> peers;
      cluster_->mini_tablet_server(i)->server()->tablet_manager()->GetTabletPeers(&peers);
      for (const auto& peer : peers) {
        if (peer->consensus() &&
            peer->consensus()->GetLeaderStatus() !=
                consensus::LeaderStatus::NOT_LEADER &&
            peer->tablet()->transaction_coordinator() &&
            peer->tablet()->transaction_coordinator()->test_count_transactions()) {
          consensus::LeaderStepDownRequestPB req;
          req.set_tablet_id(peer->tablet_id());
          consensus::LeaderStepDownResponsePB resp;
          ASSERT_OK(peer->consensus()->StepDown(&req, &resp));
        }
      }
    }
    std::this_thread::sleep_for(3s);
  }
  stopped = true;

  for (auto& thread : threads) {
    thread.join();
  }
}

class RemoteBootstrapTest : public QLTransactionTest {
 protected:
  void SetUp() override {
    FLAGS_remote_bootstrap_max_chunk_size = 1_KB;
    QLTransactionTest::SetUp();
  }
};

// Check that we do correct remote bootstrap for intents db.
// Workflow is the following:
// Shutdown TServer with index 0.
// Write some data to two remaining servers.
// Flush data and clean logs.
// Restart cluster.
// Verify that all tablets at all tservers are up and running.
// Verify that all tservers have same amount of running tablets.
// During test tear down cluster verifier will check that all servers have same data.
TEST_F_EX(QLTransactionTest, RemoteBootstrap, RemoteBootstrapTest) {
  constexpr size_t kNumWrites = 10;
  constexpr size_t kTransactionalWrites = 8;
  constexpr size_t kNumRows = 30;

  DisableTransactionTimeout();
  DisableApplyingIntents();
  FLAGS_log_min_seconds_to_retain = 1;

  cluster_->mini_tablet_server(0)->Shutdown();

  for (size_t i = 0; i != kNumWrites; ++i) {
    auto transaction = i < kTransactionalWrites ? CreateTransaction() : nullptr;
    auto session = CreateSession(transaction);
    for (size_t r = 0; r != kNumRows; ++r) {
      ASSERT_OK(WriteRow(
          session,
          KeyForTransactionAndIndex(i, r),
          ValueForTransactionAndIndex(i, r, WriteOpType::INSERT)));
    }
    if (transaction) {
      ASSERT_OK(transaction->CommitFuture().get());
    }
  }

  VerifyData(kNumWrites);

  // Wait until all tablets done writing to db.
  std::this_thread::sleep_for(5s);

  LOG(INFO) << "Flushing";
  ASSERT_OK(cluster_->FlushTablets());

  LOG(INFO) << "Clean logs";
  ASSERT_OK(cluster_->CleanTabletLogs());

  // Wait logs cleanup.
  std::this_thread::sleep_for(5s * kTimeMultiplier);

  // Shutdown to reset cached logs.
  for (int i = 1; i != cluster_->num_tablet_servers(); ++i) {
    cluster_->mini_tablet_server(i)->Shutdown();
  }

  // Start all servers. Cluster verifier should check that all tablets are synchronized.
  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    ASSERT_OK(cluster_->mini_tablet_server(i)->Start());
  }

  ASSERT_OK(WaitFor([this] { return CheckAllTabletsRunning(); }, 20s * kTimeMultiplier,
                    "All tablets running"));
}

TEST_F(QLTransactionTest, FlushIntents) {
  FLAGS_flush_rocksdb_on_shutdown = false;

  WriteData();
  WriteRows(CreateSession(), 1);

  VerifyData(2);

  ASSERT_OK(cluster_->FlushTablets(tablet::FlushMode::kSync, tablet::FlushFlags::kIntents));
  cluster_->Shutdown();
  ASSERT_OK(cluster_->StartSync());

  VerifyData(2);
}

// Test that we could init transaction after it was originally created.
TEST_F(QLTransactionTest, DelayedInit) {
  SetAtomicFlag(0ULL, &FLAGS_max_clock_skew_usec); // To avoid read restart in this test.

  auto txn1 = std::make_shared<YBTransaction>(transaction_manager_.get_ptr());
  auto txn2 = std::make_shared<YBTransaction>(transaction_manager_.get_ptr());

  auto write_session = CreateSession();
  ASSERT_OK(WriteRow(write_session, 0, 0));

  ConsistentReadPoint read_point(transaction_manager_->clock());
  read_point.SetCurrentReadTime();

  ASSERT_OK(WriteRow(write_session, 1, 1));

  ASSERT_OK(txn1->Init(IsolationLevel::SNAPSHOT_ISOLATION, read_point.GetReadTime()));
  ASSERT_OK(txn2->Init(IsolationLevel::SNAPSHOT_ISOLATION));

  ASSERT_OK(WriteRow(write_session, 2, 2));

  {
    auto read_session = CreateSession(txn1);
    auto row0 = ASSERT_RESULT(SelectRow(read_session, 0));
    ASSERT_EQ(0, row0);
    auto row1 = SelectRow(read_session, 1);
    ASSERT_TRUE(!row1.ok() && row1.status().IsNotFound()) << row1;
    auto row2 = SelectRow(read_session, 2);
    ASSERT_TRUE(!row2.ok() && row2.status().IsNotFound()) << row2;
  }

  {
    auto read_session = CreateSession(txn2);
    auto row0 = ASSERT_RESULT(SelectRow(read_session, 0));
    ASSERT_EQ(0, row0);
    auto row1 = ASSERT_RESULT(SelectRow(read_session, 1));
    ASSERT_EQ(1, row1);
    auto row2 = SelectRow(read_session, 2);
    ASSERT_TRUE(!row2.ok() && row2.status().IsNotFound()) << row2;
  }
}

} // namespace client
} // namespace yb
