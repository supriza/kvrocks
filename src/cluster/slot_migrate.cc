/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "slot_migrate.h"

#include <memory>
#include <utility>

#include "db_util.h"
#include "event_util.h"
#include "fmt/format.h"
#include "io_util.h"
#include "storage/batch_extractor.h"
#include "storage/iterator.h"
#include "sync_migrate_context.h"
#include "thread_util.h"
#include "time_util.h"
#include "types/redis_stream_base.h"

const char *errFailedToSendCommands = "failed to send commands to restore a key";
const char *errMigrationTaskCanceled = "key migration stopped due to a task cancellation";
const char *errFailedToSetImportStatus = "failed to set import status on destination node";
const char *errUnsupportedMigrationType = "unsupported migration type";

static std::map<RedisType, std::string> type_to_cmd = {
    {kRedisString, "set"}, {kRedisList, "rpush"},    {kRedisHash, "hmset"},      {kRedisSet, "sadd"},
    {kRedisZSet, "zadd"},  {kRedisBitmap, "setbit"}, {kRedisSortedint, "siadd"}, {kRedisStream, "xadd"},
};

SlotMigrator::SlotMigrator(Server *srv)
    : Database(srv->storage, kDefaultNamespace),
      srv_(srv),
      max_migration_speed_(srv->GetConfig()->migrate_speed),
      max_pipeline_size_(srv->GetConfig()->pipeline_size),
      seq_gap_limit_(srv->GetConfig()->sequence_gap),
      migrate_batch_bytes_per_sec_(srv->GetConfig()->migrate_batch_rate_limit_mb * MiB),
      migrate_batch_size_bytes_(srv->GetConfig()->migrate_batch_size_kb * KiB) {
  // Let metadata_cf_handle_ be nullptr, and get them in real time to avoid accessing invalid pointer,
  // because metadata_cf_handle_ and db_ will be destroyed if DB is reopened.
  // [Situation]:
  // 1. Start an empty slave server.
  // 2. Connect to master which has amounted of data, and trigger full synchronization.
  // 3. After replication, change slave to master and start slot migrate.
  // 4. It will occur segment fault when using metadata_cf_handle_ to create iterator of rocksdb.
  // [Reason]:
  // After full synchronization, DB will be reopened, db_ and metadata_cf_handle_ will be released.
  // Then, if we create rocksdb iterator with metadata_cf_handle_, it will go wrong.
  // [Solution]:
  // db_ and metadata_cf_handle_ will be replaced by storage_->GetDB() and storage_->GetCFHandle("metadata")
  // in all functions used in migration process.
  // [Note]:
  // This problem may exist in all functions of Database called in slot migration process.
  metadata_cf_handle_ = nullptr;

  if (srv->IsSlave()) {
    SetStopMigrationFlag(true);
  }
}

Status SlotMigrator::PerformSlotMigration(const std::string &node_id, std::string &dst_ip, int dst_port, int slot_id,
                                          SyncMigrateContext *blocking_ctx) {
  // Only one slot migration job at the same time
  int16_t no_slot = -1;
  if (!migrating_slot_.compare_exchange_strong(no_slot, static_cast<int16_t>(slot_id))) {
    return {Status::NotOK, "There is already a migrating slot"};
  }

  if (forbidden_slot_ == slot_id) {
    // Have to release migrate slot set above
    migrating_slot_ = -1;
    return {Status::NotOK, "Can't migrate slot which has been migrated"};
  }

  migration_state_ = MigrationState::kStarted;

  auto speed = srv_->GetConfig()->migrate_speed;
  auto seq_gap = srv_->GetConfig()->sequence_gap;
  auto pipeline_size = srv_->GetConfig()->pipeline_size;

  if (speed <= 0) {
    speed = 0;
  }

  if (pipeline_size <= 0) {
    pipeline_size = kDefaultMaxPipelineSize;
  }

  if (seq_gap <= 0) {
    seq_gap = kDefaultSequenceGapLimit;
  }

  if (blocking_ctx) {
    std::unique_lock<std::mutex> lock(blocking_mutex_);
    blocking_context_ = blocking_ctx;
    blocking_context_->Suspend();
  }

  dst_node_ = node_id;

  // Create migration job
  auto job = std::make_unique<SlotMigrationJob>(slot_id, dst_ip, dst_port, speed, pipeline_size, seq_gap);
  {
    std::lock_guard<std::mutex> guard(job_mutex_);
    migration_job_ = std::move(job);
    job_cv_.notify_one();
  }

  LOG(INFO) << "[migrate] Start migrating slot " << slot_id << " to " << dst_ip << ":" << dst_port;

  return Status::OK();
}

SlotMigrator::~SlotMigrator() {
  if (thread_state_ == ThreadState::Running) {
    stop_migration_ = true;
    thread_state_ = ThreadState::Terminated;
    job_cv_.notify_all();
    if (auto s = util::ThreadJoin(t_); !s) {
      LOG(WARNING) << "Slot migrating thread operation failed: " << s.Msg();
    }
  }
}

Status SlotMigrator::CreateMigrationThread() {
  t_ = GET_OR_RET(util::CreateThread("slot-migrate", [this] {
    thread_state_ = ThreadState::Running;
    this->loop();
  }));

  return Status::OK();
}

void SlotMigrator::loop() {
  while (true) {
    {
      std::unique_lock<std::mutex> ul(job_mutex_);
      job_cv_.wait(ul, [&] { return isTerminated() || migration_job_; });
    }

    if (isTerminated()) {
      clean();
      return;
    }

    LOG(INFO) << "[migrate] Migrating slot: " << migration_job_->slot_id << ", dst_ip: " << migration_job_->dst_ip
              << ", dst_port: " << migration_job_->dst_port << ", max_speed: " << migration_job_->max_speed
              << ", max_pipeline_size: " << migration_job_->max_pipeline_size;

    dst_ip_ = migration_job_->dst_ip;
    dst_port_ = migration_job_->dst_port;
    max_migration_speed_ = migration_job_->max_speed;
    max_pipeline_size_ = migration_job_->max_pipeline_size;
    seq_gap_limit_ = migration_job_->seq_gap_limit;

    runMigrationProcess();
  }
}

void SlotMigrator::runMigrationProcess() {
  current_stage_ = SlotMigrationStage::kStart;

  while (true) {
    if (isTerminated()) {
      LOG(WARNING) << "[migrate] Will stop state machine, because the thread was terminated";
      clean();
      return;
    }

    switch (current_stage_) {
      case SlotMigrationStage::kStart: {
        auto s = startMigration();
        if (s.IsOK()) {
          LOG(INFO) << "[migrate] Succeed to start migrating slot " << migrating_slot_;
          current_stage_ = SlotMigrationStage::kSnapshot;
        } else {
          LOG(ERROR) << "[migrate] Failed to start migrating slot " << migrating_slot_ << ". Error: " << s.Msg();
          current_stage_ = SlotMigrationStage::kFailed;
          resumeSyncCtx(s);
        }
        break;
      }
      case SlotMigrationStage::kSnapshot: {
        auto s = sendSnapshot();
        if (s.IsOK()) {
          current_stage_ = SlotMigrationStage::kWAL;
        } else {
          LOG(ERROR) << "[migrate] Failed to send snapshot of slot " << migrating_slot_ << ". Error: " << s.Msg();
          current_stage_ = SlotMigrationStage::kFailed;
          resumeSyncCtx(s);
        }
        break;
      }
      case SlotMigrationStage::kWAL: {
        auto s = syncWAL();
        if (s.IsOK()) {
          LOG(INFO) << "[migrate] Succeed to sync from WAL for a slot " << migrating_slot_;
          current_stage_ = SlotMigrationStage::kSuccess;
        } else {
          LOG(ERROR) << "[migrate] Failed to sync from WAL for a slot " << migrating_slot_ << ". Error: " << s.Msg();
          current_stage_ = SlotMigrationStage::kFailed;
          resumeSyncCtx(s);
        }
        break;
      }
      case SlotMigrationStage::kSuccess: {
        auto s = finishSuccessfulMigration();
        if (s.IsOK()) {
          LOG(INFO) << "[migrate] Succeed to migrate slot " << migrating_slot_;
          current_stage_ = SlotMigrationStage::kClean;
          migration_state_ = MigrationState::kSuccess;
          resumeSyncCtx(s);
        } else {
          LOG(ERROR) << "[migrate] Failed to finish a successful migration of slot " << migrating_slot_
                     << ". Error: " << s.Msg();
          current_stage_ = SlotMigrationStage::kFailed;
          resumeSyncCtx(s);
        }
        break;
      }
      case SlotMigrationStage::kFailed: {
        auto s = finishFailedMigration();
        if (!s.IsOK()) {
          LOG(ERROR) << "[migrate] Failed to finish a failed migration of slot " << migrating_slot_
                     << ". Error: " << s.Msg();
        }
        LOG(INFO) << "[migrate] Failed to migrate a slot" << migrating_slot_;
        migration_state_ = MigrationState::kFailed;
        current_stage_ = SlotMigrationStage::kClean;
        break;
      }
      case SlotMigrationStage::kClean: {
        clean();
        return;
      }
      default:
        LOG(ERROR) << "[migrate] Unexpected state for the state machine: " << static_cast<int>(current_stage_);
        clean();
        return;
    }
  }
}

Status SlotMigrator::startMigration() {
  // Get snapshot and sequence
  slot_snapshot_ = storage_->GetDB()->GetSnapshot();
  if (!slot_snapshot_) {
    return {Status::NotOK, "failed to create snapshot"};
  }

  wal_begin_seq_ = slot_snapshot_->GetSequenceNumber();
  last_send_time_ = 0;

  // Connect to the destination node
  auto result = util::SockConnect(dst_ip_, dst_port_);
  if (!result.IsOK()) {
    return {Status::NotOK, fmt::format("failed to connect to the destination node: {}", result.Msg())};
  }

  dst_fd_.Reset(*result);

  // Auth first
  std::string pass = srv_->GetConfig()->requirepass;
  if (!pass.empty()) {
    auto s = authOnDstNode(*dst_fd_, pass);
    if (!s.IsOK()) {
      return s.Prefixed("failed to authenticate on destination node");
    }
  }

  // Set destination node import status to START
  auto s = setImportStatusOnDstNode(*dst_fd_, kImportStart);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSetImportStatus);
  }

  LOG(INFO) << "[migrate] Start migrating slot " << migrating_slot_ << ", connect destination fd " << *dst_fd_;

  return Status::OK();
}

Status SlotMigrator::sendSnapshot() {
  if (srv_->GetConfig()->migrate_type == MigrationType::kRedisCommand) {
    return sendSnapshotByCmd();
  } else if (srv_->GetConfig()->migrate_type == MigrationType::kRawKeyValue) {
    return sendSnapshotByRawKV();
  }
  return {Status::NotOK, errUnsupportedMigrationType};
}

Status SlotMigrator::syncWAL() {
  if (srv_->GetConfig()->migrate_type == MigrationType::kRedisCommand) {
    return syncWALByCmd();
  } else if (srv_->GetConfig()->migrate_type == MigrationType::kRawKeyValue) {
    return syncWALByRawKV();
  }
  return {Status::NotOK, errUnsupportedMigrationType};
}

Status SlotMigrator::sendSnapshotByCmd() {
  uint64_t migrated_key_cnt = 0;
  uint64_t expired_key_cnt = 0;
  uint64_t empty_key_cnt = 0;
  std::string restore_cmds;
  int16_t slot = migrating_slot_;

  LOG(INFO) << "[migrate] Start migrating snapshot of slot " << slot;

  rocksdb::ReadOptions read_options = storage_->DefaultScanOptions();
  read_options.snapshot = slot_snapshot_;
  rocksdb::ColumnFamilyHandle *cf_handle = storage_->GetCFHandle(engine::kMetadataColumnFamilyName);
  auto iter = util::UniqueIterator(storage_->GetDB()->NewIterator(read_options, cf_handle));

  // Construct key prefix to iterate the keys belong to the target slot
  std::string prefix = ComposeSlotKeyPrefix(namespace_, slot);
  LOG(INFO) << "[migrate] Iterate keys of slot, key's prefix: " << prefix;

  // Seek to the beginning of keys start with 'prefix' and iterate all these keys
  for (iter->Seek(prefix); iter->Valid(); iter->Next()) {
    // The migrating task has to be stopped, if server role is changed from master to slave
    // or flush command (flushdb or flushall) is executed
    if (stop_migration_) {
      return {Status::NotOK, errMigrationTaskCanceled};
    }

    // Iteration is out of range
    if (!iter->key().starts_with(prefix)) {
      break;
    }

    // Get user key
    auto [_, user_key] = ExtractNamespaceKey(iter->key(), true);

    // Add key's constructed commands to restore_cmds, send pipeline or not according to task's max_pipeline_size
    auto result = migrateOneKey(user_key, iter->value(), &restore_cmds);
    if (!result.IsOK()) {
      return {Status::NotOK, fmt::format("failed to migrate a key {}: {}", user_key, result.Msg())};
    }

    if (*result == KeyMigrationResult::kMigrated) {
      LOG(INFO) << "[migrate] The key " << user_key << " successfully migrated";
      migrated_key_cnt++;
    } else if (*result == KeyMigrationResult::kExpired) {
      LOG(INFO) << "[migrate] The key " << user_key << " is expired";
      expired_key_cnt++;
    } else if (*result == KeyMigrationResult::kUnderlyingStructEmpty) {
      LOG(INFO) << "[migrate] The key " << user_key << " has no elements";
      empty_key_cnt++;
    } else {
      LOG(ERROR) << "[migrate] Migrated a key " << user_key << " with unexpected result: " << static_cast<int>(*result);
      return {Status::NotOK};
    }
  }

  // It's necessary to send commands that are still in the pipeline since the final pipeline may not be sent
  // while iterating keys because its size could be less than max_pipeline_size_
  auto s = sendCmdsPipelineIfNeed(&restore_cmds, true);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSendCommands);
  }

  LOG(INFO) << "[migrate] Succeed to migrate slot snapshot, slot: " << slot << ", Migrated keys: " << migrated_key_cnt
            << ", Expired keys: " << expired_key_cnt << ", Empty keys: " << empty_key_cnt;

  return Status::OK();
}

Status SlotMigrator::syncWALByCmd() {
  // Send incremental data from WAL circularly until new increment less than a certain amount
  auto s = syncWalBeforeForbiddingSlot();
  if (!s.IsOK()) {
    return s.Prefixed("failed to sync WAL before forbidding a slot");
  }

  setForbiddenSlot(migrating_slot_);

  // Send last incremental data
  s = syncWalAfterForbiddingSlot();
  if (!s.IsOK()) {
    return s.Prefixed("failed to sync WAL after forbidding a slot");
  }

  return Status::OK();
}

Status SlotMigrator::finishSuccessfulMigration() {
  if (stop_migration_) {
    return {Status::NotOK, errMigrationTaskCanceled};
  }

  // Set import status on the destination node to SUCCESS
  auto s = setImportStatusOnDstNode(*dst_fd_, kImportSuccess);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSetImportStatus);
  }

  std::string dst_ip_port = dst_ip_ + ":" + std::to_string(dst_port_);
  s = srv_->cluster->SetSlotMigrated(migrating_slot_, dst_ip_port);
  if (!s.IsOK()) {
    return s.Prefixed(fmt::format("failed to set slot {} as migrated to {}", migrating_slot_.load(), dst_ip_port));
  }

  migrate_failed_slot_ = -1;

  return Status::OK();
}

Status SlotMigrator::finishFailedMigration() {
  // Stop slot will forbid writing
  migrate_failed_slot_ = migrating_slot_;
  forbidden_slot_ = -1;

  // Set import status on the destination node to FAILED
  auto s = setImportStatusOnDstNode(*dst_fd_, kImportFailed);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSetImportStatus);
  }

  return Status::OK();
}

void SlotMigrator::clean() {
  LOG(INFO) << "[migrate] Clean resources of migrating slot " << migrating_slot_;
  if (slot_snapshot_) {
    storage_->GetDB()->ReleaseSnapshot(slot_snapshot_);
    slot_snapshot_ = nullptr;
  }

  current_stage_ = SlotMigrationStage::kNone;
  current_pipeline_size_ = 0;
  wal_begin_seq_ = 0;
  std::lock_guard<std::mutex> guard(job_mutex_);
  migration_job_.reset();
  dst_fd_.Reset();
  migrating_slot_ = -1;
  SetStopMigrationFlag(false);
}

Status SlotMigrator::authOnDstNode(int sock_fd, const std::string &password) {
  std::string cmd = redis::ArrayOfBulkStrings({"auth", password});
  auto s = util::SockSend(sock_fd, cmd);
  if (!s.IsOK()) {
    return s.Prefixed("failed to send AUTH command");
  }

  s = checkSingleResponse(sock_fd);
  if (!s.IsOK()) {
    return s.Prefixed("failed to check the response of AUTH command");
  }

  return Status::OK();
}

Status SlotMigrator::setImportStatusOnDstNode(int sock_fd, int status) {
  if (sock_fd <= 0) return {Status::NotOK, "invalid socket descriptor"};

  std::string cmd =
      redis::ArrayOfBulkStrings({"cluster", "import", std::to_string(migrating_slot_), std::to_string(status)});
  auto s = util::SockSend(sock_fd, cmd);
  if (!s.IsOK()) {
    return s.Prefixed("failed to send command to the destination node");
  }

  s = checkSingleResponse(sock_fd);
  if (!s.IsOK()) {
    return s.Prefixed("failed to check the response from the destination node");
  }

  return Status::OK();
}

Status SlotMigrator::checkSingleResponse(int sock_fd) { return checkMultipleResponses(sock_fd, 1); }

// Commands  |  Response            |  Instance
// ++++++++++++++++++++++++++++++++++++++++
// set          Redis::Integer         :1\r\n
// hset         Redis::SimpleString    +OK\r\n
// sadd         Redis::Integer
// zadd         Redis::Integer
// siadd        Redis::Integer
// setbit       Redis::Integer
// expire       Redis::Integer
// lpush        Redis::Integer
// rpush        Redis::Integer
// ltrim        Redis::SimpleString    -Err\r\n
// linsert      Redis::Integer
// lset         Redis::SimpleString
// hdel         Redis::Integer
// srem         Redis::Integer
// zrem         Redis::Integer
// lpop         Redis::NilString       $-1\r\n
//          or  Redis::BulkString      $1\r\n1\r\n
// rpop         Redis::NilString
//          or  Redis::BulkString
// lrem         Redis::Integer
// sirem        Redis::Integer
// del          Redis::Integer
// xadd         Redis::BulkString
// bitfield     Redis::Array           *1\r\n:0
Status SlotMigrator::checkMultipleResponses(int sock_fd, int total) {
  if (sock_fd < 0 || total <= 0) {
    return {Status::NotOK, fmt::format("invalid arguments: sock_fd={}, count={}", sock_fd, total)};
  }

  // Set socket receive timeout first
  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // Start checking response
  size_t bulk_or_array_len = 0;
  int cnt = 0;
  parser_state_ = ParserState::ArrayLen;
  UniqueEvbuf evbuf;
  while (true) {
    // Read response data from socket buffer to the event buffer
    if (evbuffer_read(evbuf.get(), sock_fd, -1) <= 0) {
      return {Status::NotOK, fmt::format("failed to read response: {}", strerror(errno))};
    }

    // Parse response data in event buffer
    bool run = true;
    while (run) {
      switch (parser_state_) {
        // Handle single string response
        case ParserState::ArrayLen: {
          UniqueEvbufReadln line(evbuf.get(), EVBUFFER_EOL_CRLF_STRICT);
          if (!line) {
            LOG(INFO) << "[migrate] Event buffer is empty, read socket again";
            run = false;
            break;
          }

          if (line[0] == '-') {
            return {Status::NotOK, fmt::format("got invalid response of length {}: {}", line.length, line.get())};
          } else if (line[0] == '$' || line[0] == '*') {
            auto parse_result = ParseInt<uint64_t>(std::string(line.get() + 1, line.length - 1), 10);
            if (!parse_result) {
              return {Status::NotOK, "protocol error: expected integer value"};
            }

            bulk_or_array_len = *parse_result;
            if (bulk_or_array_len <= 0) {
              parser_state_ = ParserState::OneRspEnd;
            } else if (line[0] == '$') {
              parser_state_ = ParserState::BulkData;
            } else {
              parser_state_ = ParserState::ArrayData;
            }
          } else if (line[0] == '+' || line[0] == ':') {
            parser_state_ = ParserState::OneRspEnd;
          } else {
            return {Status::NotOK, fmt::format("got unexpected response of length {}: {}", line.length, line.get())};
          }

          break;
        }
        // Handle bulk string response
        case ParserState::BulkData: {
          if (evbuffer_get_length(evbuf.get()) < bulk_or_array_len + 2) {
            LOG(INFO) << "[migrate] Bulk data in event buffer is not complete, read socket again";
            run = false;
            break;
          }
          // TODO(chrisZMF): Check tail '\r\n'
          evbuffer_drain(evbuf.get(), bulk_or_array_len + 2);
          bulk_or_array_len = 0;
          parser_state_ = ParserState::OneRspEnd;
          break;
        }
        case ParserState::ArrayData: {
          while (run && bulk_or_array_len > 0) {
            evbuffer_ptr ptr = evbuffer_search_eol(evbuf.get(), nullptr, nullptr, EVBUFFER_EOL_CRLF_STRICT);
            if (ptr.pos < 0) {
              LOG(INFO) << "[migrate] Array data in event buffer is not complete, read socket again";
              run = false;
              break;
            }
            evbuffer_drain(evbuf.get(), ptr.pos + 2);
            --bulk_or_array_len;
          }
          if (run) {
            parser_state_ = ParserState::OneRspEnd;
          }
          break;
        }
        case ParserState::OneRspEnd: {
          cnt++;
          if (cnt >= total) {
            return Status::OK();
          }

          parser_state_ = ParserState::ArrayLen;
          break;
        }
        default:
          break;
      }
    }
  }
}

StatusOr<KeyMigrationResult> SlotMigrator::migrateOneKey(const rocksdb::Slice &key,
                                                         const rocksdb::Slice &encoded_metadata,
                                                         std::string *restore_cmds) {
  std::string bytes = encoded_metadata.ToString();
  Metadata metadata(kRedisNone, false);
  if (auto s = metadata.Decode(bytes); !s.ok()) {
    return {Status::NotOK, s.ToString()};
  }

  if (!metadata.IsEmptyableType() && metadata.size == 0) {
    return KeyMigrationResult::kUnderlyingStructEmpty;
  }

  if (metadata.Expired()) {
    return KeyMigrationResult::kExpired;
  }

  // Construct command according to type of the key
  switch (metadata.Type()) {
    case kRedisString: {
      auto s = migrateSimpleKey(key, metadata, bytes, restore_cmds);
      if (!s.IsOK()) {
        return s.Prefixed("failed to migrate simple key");
      }
      break;
    }
    case kRedisList:
    case kRedisZSet:
    case kRedisBitmap:
    case kRedisHash:
    case kRedisSet:
    case kRedisSortedint: {
      auto s = migrateComplexKey(key, metadata, restore_cmds);
      if (!s.IsOK()) {
        return s.Prefixed("failed to migrate complex key");
      }
      break;
    }
    case kRedisStream: {
      StreamMetadata stream_md(false);
      if (auto s = stream_md.Decode(bytes); !s.ok()) {
        return {Status::NotOK, s.ToString()};
      }

      auto s = migrateStream(key, stream_md, restore_cmds);
      if (!s.IsOK()) {
        return s.Prefixed("failed to migrate stream key");
      }
      break;
    }
    default:
      break;
  }

  return KeyMigrationResult::kMigrated;
}

Status SlotMigrator::migrateSimpleKey(const rocksdb::Slice &key, const Metadata &metadata, const std::string &bytes,
                                      std::string *restore_cmds) {
  std::vector<std::string> command = {"SET", key.ToString(), bytes.substr(Metadata::GetOffsetAfterExpire(bytes[0]))};
  if (metadata.expire > 0) {
    command.emplace_back("PXAT");
    command.emplace_back(std::to_string(metadata.expire));
  }
  *restore_cmds += redis::ArrayOfBulkStrings(command);
  current_pipeline_size_++;

  // Check whether pipeline needs to be sent
  // TODO(chrisZMF): Resend data if failed to send data
  auto s = sendCmdsPipelineIfNeed(restore_cmds, false);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSendCommands);
  }

  return Status::OK();
}

Status SlotMigrator::migrateComplexKey(const rocksdb::Slice &key, const Metadata &metadata, std::string *restore_cmds) {
  std::string cmd;
  cmd = type_to_cmd[metadata.Type()];

  std::vector<std::string> user_cmd = {cmd, key.ToString()};
  rocksdb::ReadOptions read_options = storage_->DefaultScanOptions();
  read_options.snapshot = slot_snapshot_;
  // Should use th raw db iterator to avoid reading uncommitted writes in transaction mode
  auto iter = util::UniqueIterator(storage_->GetDB()->NewIterator(read_options));

  // Construct key prefix to iterate values of the complex type user key
  std::string slot_key = AppendNamespacePrefix(key);
  std::string prefix_subkey = InternalKey(slot_key, "", metadata.version, true).Encode();
  int item_count = 0;

  for (iter->Seek(prefix_subkey); iter->Valid(); iter->Next()) {
    if (stop_migration_) {
      return {Status::NotOK, errMigrationTaskCanceled};
    }

    if (!iter->key().starts_with(prefix_subkey)) {
      break;
    }

    // Parse values of the complex key
    // InternalKey is adopted to get complex key's value from the formatted key return by iterator of rocksdb
    InternalKey inkey(iter->key(), true);
    switch (metadata.Type()) {
      case kRedisSet: {
        user_cmd.emplace_back(inkey.GetSubKey().ToString());
        break;
      }
      case kRedisSortedint: {
        auto id = DecodeFixed64(inkey.GetSubKey().ToString().data());
        user_cmd.emplace_back(std::to_string(id));
        break;
      }
      case kRedisZSet: {
        auto score = DecodeDouble(iter->value().ToString().data());
        user_cmd.emplace_back(util::Float2String(score));
        user_cmd.emplace_back(inkey.GetSubKey().ToString());
        break;
      }
      case kRedisBitmap: {
        auto s = migrateBitmapKey(inkey, &iter, &user_cmd, restore_cmds);
        if (!s.IsOK()) {
          return s.Prefixed("failed to migrate bitmap key");
        }
        break;
      }
      case kRedisHash: {
        user_cmd.emplace_back(inkey.GetSubKey().ToString());
        user_cmd.emplace_back(iter->value().ToString());
        break;
      }
      case kRedisList: {
        user_cmd.emplace_back(iter->value().ToString());
        break;
      }
      default:
        break;
    }

    // Check item count
    // Exclude bitmap because it does not have hmset-like command
    if (metadata.Type() != kRedisBitmap) {
      item_count++;
      if (item_count >= kMaxItemsInCommand) {
        *restore_cmds += redis::ArrayOfBulkStrings(user_cmd);
        current_pipeline_size_++;
        item_count = 0;
        // Have to clear saved items
        user_cmd.erase(user_cmd.begin() + 2, user_cmd.end());

        // Send commands if the pipeline contains enough of them
        auto s = sendCmdsPipelineIfNeed(restore_cmds, false);
        if (!s.IsOK()) {
          return s.Prefixed(errFailedToSendCommands);
        }
      }
    }
  }

  // Have to check the item count of the last command list
  if (item_count % kMaxItemsInCommand != 0) {
    *restore_cmds += redis::ArrayOfBulkStrings(user_cmd);
    current_pipeline_size_++;
  }

  // Add TTL for complex key
  if (metadata.expire > 0) {
    *restore_cmds += redis::ArrayOfBulkStrings({"PEXPIREAT", key.ToString(), std::to_string(metadata.expire)});
    current_pipeline_size_++;
  }

  // Send commands if the pipeline contains enough of them
  auto s = sendCmdsPipelineIfNeed(restore_cmds, false);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSendCommands);
  }

  return Status::OK();
}

Status SlotMigrator::migrateStream(const Slice &key, const StreamMetadata &metadata, std::string *restore_cmds) {
  rocksdb::ReadOptions read_options = storage_->DefaultScanOptions();
  read_options.snapshot = slot_snapshot_;
  // Should use th raw db iterator to avoid reading uncommitted writes in transaction mode
  auto iter = util::UniqueIterator(
      storage_->GetDB()->NewIterator(read_options, storage_->GetCFHandle(engine::kStreamColumnFamilyName)));

  std::string ns_key = AppendNamespacePrefix(key);
  // Construct key prefix to iterate values of the stream
  std::string prefix_key = InternalKey(ns_key, "", metadata.version, true).Encode();

  std::vector<std::string> user_cmd = {type_to_cmd[metadata.Type()], key.ToString()};

  for (iter->Seek(prefix_key); iter->Valid(); iter->Next()) {
    if (stop_migration_) {
      return {Status::NotOK, errMigrationTaskCanceled};
    }

    if (!iter->key().starts_with(prefix_key)) {
      break;
    }

    auto s = WriteBatchExtractor::ExtractStreamAddCommand(true, iter->key(), iter->value(), &user_cmd);
    if (!s.IsOK()) {
      return s;
    }
    *restore_cmds += redis::ArrayOfBulkStrings(user_cmd);
    current_pipeline_size_++;

    user_cmd.erase(user_cmd.begin() + 2, user_cmd.end());

    s = sendCmdsPipelineIfNeed(restore_cmds, false);
    if (!s.IsOK()) {
      return s.Prefixed(errFailedToSendCommands);
    }
  }

  // commands like XTRIM and XDEL affect stream's metadata, but we use only XADD for a slot migration
  // XSETID is used to adjust stream's info on the destination node according to the current values on the source
  *restore_cmds += redis::ArrayOfBulkStrings({"XSETID", key.ToString(), metadata.last_generated_id.ToString(),
                                              "ENTRIESADDED", std::to_string(metadata.entries_added), "MAXDELETEDID",
                                              metadata.max_deleted_entry_id.ToString()});
  current_pipeline_size_++;

  // Add TTL
  if (metadata.expire > 0) {
    *restore_cmds += redis::ArrayOfBulkStrings({"PEXPIREAT", key.ToString(), std::to_string(metadata.expire)});
    current_pipeline_size_++;
  }

  auto s = sendCmdsPipelineIfNeed(restore_cmds, false);
  if (!s.IsOK()) {
    return s.Prefixed(errFailedToSendCommands);
  }

  return Status::OK();
}

Status SlotMigrator::migrateBitmapKey(const InternalKey &inkey, std::unique_ptr<rocksdb::Iterator> *iter,
                                      std::vector<std::string> *user_cmd, std::string *restore_cmds) {
  std::string index_str = inkey.GetSubKey().ToString();
  std::string fragment = (*iter)->value().ToString();
  auto parse_result = ParseInt<int>(index_str, 10);
  if (!parse_result) {
    return {Status::RedisParseErr, "index is not a valid integer"};
  }

  uint32_t index = *parse_result;

  // Bitmap does not have hmset-like command
  // TODO(chrisZMF): Use hmset-like command for efficiency
  for (int byte_idx = 0; byte_idx < static_cast<int>(fragment.size()); byte_idx++) {
    if (fragment[byte_idx] & 0xff) {
      for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
        if (fragment[byte_idx] & (1 << bit_idx)) {
          uint32_t offset = (index * 8) + (byte_idx * 8) + bit_idx;
          user_cmd->emplace_back(std::to_string(offset));
          user_cmd->emplace_back("1");
          *restore_cmds += redis::ArrayOfBulkStrings(*user_cmd);
          current_pipeline_size_++;
          user_cmd->erase(user_cmd->begin() + 2, user_cmd->end());
        }
      }

      auto s = sendCmdsPipelineIfNeed(restore_cmds, false);
      if (!s.IsOK()) {
        return s.Prefixed(errFailedToSendCommands);
      }
    }
  }

  return Status::OK();
}

Status SlotMigrator::sendCmdsPipelineIfNeed(std::string *commands, bool need) {
  if (stop_migration_) {
    return {Status::NotOK, errMigrationTaskCanceled};
  }

  // Check pipeline
  if (!need && current_pipeline_size_ < max_pipeline_size_) {
    return Status::OK();
  }

  if (current_pipeline_size_ == 0) {
    LOG(INFO) << "[migrate] No commands to send";
    return Status::OK();
  }

  applyMigrationSpeedLimit();

  auto s = util::SockSend(*dst_fd_, *commands);
  if (!s.IsOK()) {
    return s.Prefixed("failed to write data to a socket");
  }

  last_send_time_ = util::GetTimeStampUS();

  s = checkMultipleResponses(*dst_fd_, current_pipeline_size_);
  if (!s.IsOK()) {
    return s.Prefixed("wrong response from the destination node");
  }

  // Clear commands and running pipeline
  commands->clear();
  current_pipeline_size_ = 0;

  return Status::OK();
}

void SlotMigrator::setForbiddenSlot(int16_t slot) {
  LOG(INFO) << "[migrate] Setting forbidden slot " << slot;
  // Block server to set forbidden slot
  uint64_t during = util::GetTimeStampUS();
  {
    auto exclusivity = srv_->WorkExclusivityGuard();
    forbidden_slot_ = slot;
  }
  during = util::GetTimeStampUS() - during;
  LOG(INFO) << "[migrate] To set forbidden slot, server was blocked for " << during << "us";
}

void SlotMigrator::ReleaseForbiddenSlot() {
  LOG(INFO) << "[migrate] Release forbidden slot " << forbidden_slot_;
  forbidden_slot_ = -1;
}

void SlotMigrator::applyMigrationSpeedLimit() const {
  if (max_migration_speed_ > 0) {
    uint64_t current_time = util::GetTimeStampUS();
    uint64_t per_request_time = 1000000 * max_pipeline_size_ / max_migration_speed_;
    if (per_request_time == 0) {
      per_request_time = 1;
    }
    if (last_send_time_ + per_request_time > current_time) {
      uint64_t during = last_send_time_ + per_request_time - current_time;
      LOG(INFO) << "[migrate] Sleep to limit migration speed for: " << during;
      std::this_thread::sleep_for(std::chrono::microseconds(during));
    }
  }
}

Status SlotMigrator::generateCmdsFromBatch(rocksdb::BatchResult *batch, std::string *commands) {
  // Iterate batch to get keys and construct commands for keys
  WriteBatchExtractor write_batch_extractor(storage_->IsSlotIdEncoded(), migrating_slot_, false);
  rocksdb::Status status = batch->writeBatchPtr->Iterate(&write_batch_extractor);
  if (!status.ok()) {
    LOG(ERROR) << "[migrate] Failed to parse write batch, Err: " << status.ToString();
    return {Status::NotOK};
  }

  // Get all constructed commands
  auto resp_commands = write_batch_extractor.GetRESPCommands();
  for (const auto &iter : *resp_commands) {
    for (const auto &it : iter.second) {
      *commands += it;
      current_pipeline_size_++;
    }
  }

  return Status::OK();
}

Status SlotMigrator::migrateIncrementData(std::unique_ptr<rocksdb::TransactionLogIterator> *iter, uint64_t end_seq) {
  if (!(*iter) || !(*iter)->Valid()) {
    LOG(ERROR) << "[migrate] WAL iterator is invalid";
    return {Status::NotOK};
  }

  uint64_t next_seq = wal_begin_seq_ + 1;
  std::string commands;

  while (true) {
    if (stop_migration_) {
      LOG(ERROR) << "[migrate] Migration task end during migrating WAL data";
      return {Status::NotOK};
    }

    auto batch = (*iter)->GetBatch();
    if (batch.sequence != next_seq) {
      LOG(ERROR) << "[migrate] WAL iterator is discrete, some seq might be lost"
                 << ", expected sequence: " << next_seq << ", but got sequence: " << batch.sequence;
      return {Status::NotOK};
    }

    // Generate commands by iterating write batch
    auto s = generateCmdsFromBatch(&batch, &commands);
    if (!s.IsOK()) {
      LOG(ERROR) << "[migrate] Failed to generate commands from write batch";
      return {Status::NotOK};
    }

    // Check whether command pipeline should be sent
    s = sendCmdsPipelineIfNeed(&commands, false);
    if (!s.IsOK()) {
      LOG(ERROR) << "[migrate] Failed to send WAL commands pipeline";
      return {Status::NotOK};
    }

    next_seq = batch.sequence + batch.writeBatchPtr->Count();
    if (next_seq > end_seq) {
      LOG(INFO) << "[migrate] Migrate incremental data an epoch OK, seq from " << wal_begin_seq_ << ", to " << end_seq;
      break;
    }

    (*iter)->Next();
    if (!(*iter)->Valid()) {
      LOG(ERROR) << "[migrate] WAL iterator is invalid, expected end seq: " << end_seq << ", next seq: " << next_seq;
      return {Status::NotOK};
    }
  }

  // Send the left data of this epoch
  auto s = sendCmdsPipelineIfNeed(&commands, true);
  if (!s.IsOK()) {
    LOG(ERROR) << "[migrate] Failed to send WAL last commands in pipeline";
    return {Status::NotOK};
  }

  return Status::OK();
}

Status SlotMigrator::syncWalBeforeForbiddingSlot() {
  uint32_t count = 0;

  while (count < kMaxLoopTimes) {
    uint64_t latest_seq = storage_->GetDB()->GetLatestSequenceNumber();
    uint64_t gap = latest_seq - wal_begin_seq_;
    if (gap <= static_cast<uint64_t>(seq_gap_limit_)) {
      LOG(INFO) << "[migrate] Incremental data sequence: " << gap << ", less than limit: " << seq_gap_limit_
                << ", go to set forbidden slot";
      break;
    }

    std::unique_ptr<rocksdb::TransactionLogIterator> iter = nullptr;
    auto s = storage_->GetWALIter(wal_begin_seq_ + 1, &iter);
    if (!s.IsOK()) {
      LOG(ERROR) << "[migrate] Failed to generate WAL iterator before setting forbidden slot"
                 << ", Err: " << s.Msg();
      return {Status::NotOK};
    }

    // Iterate wal and migrate data
    s = migrateIncrementData(&iter, latest_seq);
    if (!s.IsOK()) {
      LOG(ERROR) << "[migrate] Failed to migrate WAL data before setting forbidden slot";
      return {Status::NotOK};
    }

    wal_begin_seq_ = latest_seq;
    count++;
  }

  LOG(INFO) << "[migrate] Succeed to migrate incremental data before setting forbidden slot, end epoch: " << count;
  return Status::OK();
}

Status SlotMigrator::syncWalAfterForbiddingSlot() {
  uint64_t latest_seq = storage_->GetDB()->GetLatestSequenceNumber();

  // No incremental data
  if (latest_seq <= wal_begin_seq_) return Status::OK();

  // Get WAL iter
  std::unique_ptr<rocksdb::TransactionLogIterator> iter = nullptr;
  auto s = storage_->GetWALIter(wal_begin_seq_ + 1, &iter);
  if (!s.IsOK()) {
    LOG(ERROR) << "[migrate] Failed to generate WAL iterator after setting forbidden slot"
               << ", Err: " << s.Msg();
    return {Status::NotOK};
  }

  // Send incremental data
  s = migrateIncrementData(&iter, latest_seq);
  if (!s.IsOK()) {
    LOG(ERROR) << "[migrate] Failed to migrate WAL data after setting forbidden slot";
    return {Status::NotOK};
  }

  return Status::OK();
}

void SlotMigrator::GetMigrationInfo(std::string *info) const {
  info->clear();
  if (migrating_slot_ < 0 && forbidden_slot_ < 0 && migrate_failed_slot_ < 0) {
    return;
  }

  int16_t slot = -1;
  std::string task_state;

  switch (migration_state_.load()) {
    case MigrationState::kNone:
      task_state = "none";
      break;
    case MigrationState::kStarted:
      task_state = "start";
      slot = migrating_slot_;
      break;
    case MigrationState::kSuccess:
      task_state = "success";
      slot = forbidden_slot_;
      break;
    case MigrationState::kFailed:
      task_state = "fail";
      slot = migrate_failed_slot_;
      break;
    default:
      break;
  }

  *info =
      fmt::format("migrating_slot: {}\r\ndestination_node: {}\r\nmigrating_state: {}\r\n", slot, dst_node_, task_state);
}

void SlotMigrator::CancelSyncCtx() {
  std::unique_lock<std::mutex> lock(blocking_mutex_);
  blocking_context_ = nullptr;
}

void SlotMigrator::resumeSyncCtx(const Status &migrate_result) {
  std::unique_lock<std::mutex> lock(blocking_mutex_);
  if (blocking_context_) {
    blocking_context_->Resume(migrate_result);

    blocking_context_ = nullptr;
  }
}

Status SlotMigrator::sendMigrationBatch(BatchSender *batch) {
  // user may dynamically change some configs, apply it when send data
  batch->SetMaxBytes(migrate_batch_size_bytes_);
  batch->SetBytesPerSecond(migrate_batch_bytes_per_sec_);
  return batch->Send();
}

Status SlotMigrator::sendSnapshotByRawKV() {
  uint64_t start_ts = util::GetTimeStampMS();
  LOG(INFO) << "[migrate] Migrating snapshot of slot " << migrating_slot_ << " by raw key value";

  rocksdb::ReadOptions read_options = storage_->DefaultScanOptions();
  read_options.snapshot = slot_snapshot_;
  engine::DBIterator iter(storage_, read_options);
  auto prefix = ComposeSlotKeyPrefix(namespace_, migrating_slot_);

  BatchSender batch_sender(*dst_fd_, migrate_batch_size_bytes_, migrate_batch_bytes_per_sec_);

  for (iter.Seek(prefix); iter.Valid() && iter.Key().starts_with(prefix); iter.Next()) {
    auto redis_type = iter.Type();
    std::string log_data;
    if (redis_type == RedisType::kRedisList) {
      redis::WriteBatchLogData batch_log_data(redis_type, {std::to_string(RedisCommand::kRedisCmdRPush)});
      log_data = batch_log_data.Encode();
    } else {
      redis::WriteBatchLogData batch_log_data(redis_type);
      log_data = batch_log_data.Encode();
    }
    batch_sender.SetPrefixLogData(log_data);

    GET_OR_RET(batch_sender.Put(storage_->GetCFHandle(engine::kMetadataColumnFamilyName), iter.Key(), iter.Value()));

    auto subkey_iter = iter.GetSubKeyIterator();
    if (!subkey_iter) {
      continue;
    }

    for (subkey_iter->Seek(); subkey_iter->Valid(); subkey_iter->Next()) {
      GET_OR_RET(batch_sender.Put(subkey_iter->ColumnFamilyHandle(), subkey_iter->Key(), subkey_iter->Value()));

      if (redis_type == RedisType::kRedisZSet) {
        InternalKey internal_key(subkey_iter->Key(), storage_->IsSlotIdEncoded());
        auto score_key = subkey_iter->Value().ToString();
        score_key.append(subkey_iter->UserKey().ToString());
        auto score_key_bytes =
            InternalKey(iter.Key(), score_key, internal_key.GetVersion(), storage_->IsSlotIdEncoded()).Encode();
        GET_OR_RET(batch_sender.Put(storage_->GetCFHandle(kColumnFamilyIDZSetScore), score_key_bytes, Slice()));
      }

      if (batch_sender.IsFull()) {
        GET_OR_RET(sendMigrationBatch(&batch_sender));
      }
    }

    if (batch_sender.IsFull()) {
      GET_OR_RET(sendMigrationBatch(&batch_sender));
    }
  }

  GET_OR_RET(sendMigrationBatch(&batch_sender));

  auto elapsed = util::GetTimeStampMS() - start_ts;
  LOG(INFO) << fmt::format(
      "[migrate] Succeed to migrate snapshot, slot: {}, elapsed: {} ms, "
      "sent: {} bytes, rate: {:.2f} kb/s, batches: {}, entries: {}",
      migrating_slot_.load(), elapsed, batch_sender.GetSentBytes(), batch_sender.GetRate(start_ts),
      batch_sender.GetSentBatchesNum(), batch_sender.GetEntriesNum());

  return Status::OK();
}

Status SlotMigrator::syncWALByRawKV() {
  uint64_t start_ts = util::GetTimeStampMS();
  LOG(INFO) << "[migrate] Syncing WAL of slot " << migrating_slot_ << " by raw key value";
  BatchSender batch_sender(*dst_fd_, migrate_batch_size_bytes_, migrate_batch_bytes_per_sec_);

  int epoch = 1;
  uint64_t wal_incremental_seq = 0;

  while (epoch <= kMaxLoopTimes) {
    if (catchUpIncrementalWAL()) {
      break;
    }
    wal_incremental_seq = storage_->GetDB()->GetLatestSequenceNumber();
    auto s = migrateIncrementalDataByRawKV(wal_incremental_seq, &batch_sender);
    if (!s.IsOK()) {
      return {Status::NotOK, fmt::format("migrate incremental data failed, {}", s.Msg())};
    }
    LOG(INFO) << fmt::format("[migrate] Migrated incremental data, epoch: {}, seq from {} to {}", epoch, wal_begin_seq_,
                             wal_incremental_seq);
    wal_begin_seq_ = wal_incremental_seq;
    epoch++;
  }

  setForbiddenSlot(migrating_slot_);

  wal_incremental_seq = storage_->GetDB()->GetLatestSequenceNumber();
  if (wal_incremental_seq > wal_begin_seq_) {
    auto s = migrateIncrementalDataByRawKV(wal_incremental_seq, &batch_sender);
    if (!s.IsOK()) {
      return {Status::NotOK, fmt::format("migrate last incremental data failed, {}", s.Msg())};
    }
    LOG(INFO) << fmt::format("[migrate] Migrated last incremental data after set forbidden slot, seq from {} to {}",
                             wal_begin_seq_, wal_incremental_seq);
  }

  auto elapsed = util::GetTimeStampMS() - start_ts;
  LOG(INFO) << fmt::format(
      "[migrate] Succeed to migrate incremental data, slot: {}, elapsed: {} ms, "
      "sent: {} bytes, rate: {:.2f} kb/s, batches: {}, entries: {}",
      migrating_slot_.load(), elapsed, batch_sender.GetSentBytes(), batch_sender.GetRate(start_ts),
      batch_sender.GetSentBatchesNum(), batch_sender.GetEntriesNum());

  return Status::OK();
}

bool SlotMigrator::catchUpIncrementalWAL() {
  uint64_t gap = storage_->GetDB()->GetLatestSequenceNumber() - wal_begin_seq_;
  if (gap <= seq_gap_limit_) {
    LOG(INFO) << fmt::format("[migrate] Incremental data sequence gap: {}, less than limit: {}, set forbidden slot: {}",
                             gap, seq_gap_limit_, migrating_slot_.load());
    return true;
  }
  return false;
}

Status SlotMigrator::migrateIncrementalDataByRawKV(uint64_t end_seq, BatchSender *batch_sender) {
  engine::WALIterator wal_iter(storage_, migrating_slot_);
  uint64_t start_seq = wal_begin_seq_ + 1;
  for (wal_iter.Seek(start_seq); wal_iter.Valid(); wal_iter.Next()) {
    if (wal_iter.NextSequenceNumber() > end_seq + 1) {
      break;
    }
    auto item = wal_iter.Item();
    switch (item.type) {
      case engine::WALItem::Type::kTypeLogData: {
        GET_OR_RET(batch_sender->PutLogData(item.key));
        break;
      }
      case engine::WALItem::Type::kTypePut: {
        GET_OR_RET(batch_sender->Put(storage_->GetCFHandle(static_cast<ColumnFamilyID>(item.column_family_id)),
                                     item.key, item.value));
        break;
      }
      case engine::WALItem::Type::kTypeDelete: {
        GET_OR_RET(
            batch_sender->Delete(storage_->GetCFHandle(static_cast<ColumnFamilyID>(item.column_family_id)), item.key));
        break;
      }
      case engine::WALItem::Type::kTypeDeleteRange: {
        // Do nothing in DeleteRange due to it might cross multiple slots. It's only used in
        // FLUSHDB/FLUSHALL commands for now and maybe we can disable them while migrating.
      }
      default:
        break;
    }
    if (batch_sender->IsFull()) {
      GET_OR_RET(sendMigrationBatch(batch_sender));
    }
  }

  // send the remaining data
  return sendMigrationBatch(batch_sender);
}
