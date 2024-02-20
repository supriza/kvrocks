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

#include "redis_string.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "parse_util.h"
#include "storage/redis_metadata.h"
#include "time_util.h"

namespace redis {

std::vector<rocksdb::Status> String::getRawValues(const std::vector<Slice> &keys,
                                                  std::vector<std::string> *raw_values) {
  raw_values->clear();

  rocksdb::ReadOptions read_options = storage_->DefaultMultiGetOptions();
  LatestSnapShot ss(storage_);
  read_options.snapshot = ss.GetSnapShot();
  raw_values->resize(keys.size());
  std::vector<rocksdb::Status> statuses(keys.size());
  std::vector<rocksdb::PinnableSlice> pin_values(keys.size());
  storage_->MultiGet(read_options, metadata_cf_handle_, keys.size(), keys.data(), pin_values.data(), statuses.data());
  for (size_t i = 0; i < keys.size(); i++) {
    if (!statuses[i].ok()) continue;
    (*raw_values)[i].assign(pin_values[i].data(), pin_values[i].size());
    Metadata metadata(kRedisNone, false);
    Slice slice = (*raw_values)[i];
    auto s = ParseMetadata({kRedisString}, &slice, &metadata);
    if (!s.ok()) {
      statuses[i] = s;
      (*raw_values)[i].clear();
      continue;
    }
  }
  return statuses;
}

rocksdb::Status String::getRawValue(const std::string &ns_key, std::string *raw_value) {
  raw_value->clear();

  auto s = GetRawMetadata(ns_key, raw_value);
  if (!s.ok()) return s;

  Metadata metadata(kRedisNone, false);
  Slice slice = *raw_value;
  return ParseMetadata({kRedisString}, &slice, &metadata);
}

rocksdb::Status String::getValueAndExpire(const std::string &ns_key, std::string *value, uint64_t *expire) {
  value->clear();

  std::string raw_value;
  auto s = getRawValue(ns_key, &raw_value);
  if (!s.ok()) return s;

  size_t offset = Metadata::GetOffsetAfterExpire(raw_value[0]);
  *value = raw_value.substr(offset);

  if (expire) {
    Metadata metadata(kRedisString, false);
    s = metadata.Decode(raw_value);
    if (!s.ok()) return s;
    *expire = metadata.expire;
  }
  return rocksdb::Status::OK();
}

rocksdb::Status String::getValue(const std::string &ns_key, std::string *value) {
  return getValueAndExpire(ns_key, value, nullptr);
}

std::vector<rocksdb::Status> String::getValues(const std::vector<Slice> &ns_keys, std::vector<std::string> *values) {
  auto statuses = getRawValues(ns_keys, values);
  for (size_t i = 0; i < ns_keys.size(); i++) {
    if (!statuses[i].ok()) continue;
    size_t offset = Metadata::GetOffsetAfterExpire((*values)[i][0]);
    (*values)[i] = (*values)[i].substr(offset, (*values)[i].size() - offset);
  }
  return statuses;
}

rocksdb::Status String::updateRawValue(const std::string &ns_key, const std::string &raw_value) {
  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisString);
  batch->PutLogData(log_data.Encode());
  batch->Put(metadata_cf_handle_, ns_key, raw_value);
  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

rocksdb::Status String::Append(const std::string &user_key, const std::string &value, uint64_t *new_size) {
  *new_size = 0;
  std::string ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  std::string raw_value;
  rocksdb::Status s = getRawValue(ns_key, &raw_value);
  if (!s.ok() && !s.IsNotFound()) return s;
  if (s.IsNotFound()) {
    Metadata metadata(kRedisString, false);
    metadata.Encode(&raw_value);
  }
  raw_value.append(value);
  *new_size = raw_value.size() - Metadata::GetOffsetAfterExpire(raw_value[0]);
  return updateRawValue(ns_key, raw_value);
}

std::vector<rocksdb::Status> String::MGet(const std::vector<Slice> &keys, std::vector<std::string> *values) {
  std::vector<std::string> ns_keys;
  ns_keys.reserve(keys.size());
  for (const auto &key : keys) {
    std::string ns_key = AppendNamespacePrefix(key);
    ns_keys.emplace_back(ns_key);
  }
  std::vector<Slice> slice_keys;
  slice_keys.reserve(ns_keys.size());
  for (const auto &ns_key : ns_keys) {
    slice_keys.emplace_back(ns_key);
  }
  return getValues(slice_keys, values);
}

rocksdb::Status String::Get(const std::string &user_key, std::string *value) {
  std::string ns_key = AppendNamespacePrefix(user_key);
  return getValue(ns_key, value);
}

rocksdb::Status String::GetEx(const std::string &user_key, std::string *value, uint64_t ttl, bool persist) {
  uint64_t expire = 0;
  if (ttl > 0) {
    uint64_t now = util::GetTimeStampMS();
    expire = now + ttl;
  }
  std::string ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  rocksdb::Status s = getValue(ns_key, value);
  if (!s.ok()) return s;

  std::string raw_data;
  Metadata metadata(kRedisString, false);
  if (ttl > 0 || persist) {
    metadata.expire = expire;
  } else {
    // If there is no ttl or persist is false, then skip the following updates.
    return rocksdb::Status::OK();
  }
  metadata.Encode(&raw_data);
  raw_data.append(value->data(), value->size());
  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisString);
  batch->PutLogData(log_data.Encode());
  batch->Put(metadata_cf_handle_, ns_key, raw_data);
  s = storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
  if (!s.ok()) return s;
  return rocksdb::Status::OK();
}

rocksdb::Status String::GetSet(const std::string &user_key, const std::string &new_value,
                               std::optional<std::string> &old_value) {
  auto s = Set(user_key, new_value, {/*ttl=*/0, StringSetType::NONE, /*get=*/true, /*keep_ttl=*/false}, old_value);
  return s;
}
rocksdb::Status String::GetDel(const std::string &user_key, std::string *value) {
  std::string ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  rocksdb::Status s = getValue(ns_key, value);
  if (!s.ok()) return s;

  return storage_->Delete(storage_->DefaultWriteOptions(), metadata_cf_handle_, ns_key);
}

rocksdb::Status String::Set(const std::string &user_key, const std::string &value) {
  std::vector<StringPair> pairs{StringPair{user_key, value}};
  return MSet(pairs, /*ttl=*/0, /*lock=*/true);
}

rocksdb::Status String::Set(const std::string &user_key, const std::string &value, StringSetArgs args,
                            std::optional<std::string> &ret) {
  uint64_t expire = 0;
  std::string ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  bool need_old_value = args.type != StringSetType::NONE || args.get || args.keep_ttl;
  if (need_old_value) {
    std::string old_value;
    uint64_t old_expire = 0;
    auto s = getValueAndExpire(ns_key, &old_value, &old_expire);
    if (!s.ok() && !s.IsNotFound() && !s.IsInvalidArgument()) return s;
    // GET option
    if (args.get) {
      if (s.IsInvalidArgument()) {
        return s;
      }
      if (s.IsNotFound()) {
        // if GET option given, the key didn't exist before: return nil
        ret = std::nullopt;
      } else {
        // if GET option given: return The previous value of the key.
        ret = std::make_optional(old_value);
      }
    }
    // NX/XX option
    if (args.type == StringSetType::NX && s.ok()) {
      // if NX option given, the key already exist: return nil
      if (!args.get) ret = std::nullopt;
      return rocksdb::Status::OK();
    } else if (args.type == StringSetType::XX && s.IsNotFound()) {
      // if XX option given, the key didn't exist before: return nil
      if (!args.get) ret = std::nullopt;
      return rocksdb::Status::OK();
    } else {
      // if GET option not given, make ret not nil
      if (!args.get) ret = "";
    }
    if (s.ok() && args.keep_ttl) {
      // if KEEPTTL option given, use the old ttl
      expire = old_expire;
    }
  } else {
    // if no option given, make ret not nil
    if (!args.get) ret = "";
  }

  // Handle expire time
  if (args.ttl > 0) {
    uint64_t now = util::GetTimeStampMS();
    expire = now + args.ttl;
  }

  // Create new value
  std::string new_raw_value;
  Metadata metadata(kRedisString, false);
  metadata.expire = expire;
  metadata.Encode(&new_raw_value);
  new_raw_value.append(value);
  return updateRawValue(ns_key, new_raw_value);
}

rocksdb::Status String::SetEX(const std::string &user_key, const std::string &value, uint64_t ttl) {
  std::optional<std::string> ret;
  return Set(user_key, value, {ttl, StringSetType::NONE, /*get=*/false, /*keep_ttl=*/false}, ret);
}

rocksdb::Status String::SetNX(const std::string &user_key, const std::string &value, uint64_t ttl, bool *flag) {
  std::optional<std::string> ret;
  auto s = Set(user_key, value, {ttl, StringSetType::NX, /*get=*/false, /*keep_ttl=*/false}, ret);
  *flag = ret.has_value();
  return s;
}

rocksdb::Status String::SetXX(const std::string &user_key, const std::string &value, uint64_t ttl, bool *flag) {
  std::optional<std::string> ret;
  auto s = Set(user_key, value, {ttl, StringSetType::XX, /*get=*/false, /*keep_ttl=*/false}, ret);
  *flag = ret.has_value();
  return s;
}

rocksdb::Status String::SetRange(const std::string &user_key, size_t offset, const std::string &value,
                                 uint64_t *new_size) {
  std::string ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  std::string raw_value;
  rocksdb::Status s = getRawValue(ns_key, &raw_value);
  if (!s.ok() && !s.IsNotFound()) return s;

  if (s.IsNotFound()) {
    // Return 0 directly instead of storing an empty key when set nothing on a non-existing string.
    if (value.empty()) {
      *new_size = 0;
      return rocksdb::Status::OK();
    }

    Metadata metadata(kRedisString, false);
    metadata.Encode(&raw_value);
  }

  size_t size = raw_value.size();
  size_t header_offset = Metadata::GetOffsetAfterExpire(raw_value[0]);
  offset += header_offset;
  if (offset > size) {
    // padding the value with zero byte while offset is longer than value size
    size_t paddings = offset - size;
    raw_value.append(paddings, '\0');
  }
  if (offset + value.size() >= size) {
    raw_value = raw_value.substr(0, offset);
    raw_value.append(value);
  } else {
    for (size_t i = 0; i < value.size(); i++) {
      raw_value[offset + i] = value[i];
    }
  }
  *new_size = raw_value.size() - header_offset;
  return updateRawValue(ns_key, raw_value);
}

rocksdb::Status String::IncrBy(const std::string &user_key, int64_t increment, int64_t *new_value) {
  std::string ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  std::string raw_value;
  rocksdb::Status s = getRawValue(ns_key, &raw_value);
  if (!s.ok() && !s.IsNotFound()) return s;
  if (s.IsNotFound()) {
    Metadata metadata(kRedisString, false);
    metadata.Encode(&raw_value);
  }

  size_t offset = Metadata::GetOffsetAfterExpire(raw_value[0]);
  std::string value = raw_value.substr(offset);
  int64_t n = 0;
  if (!value.empty()) {
    auto parse_result = ParseInt<int64_t>(value, 10);
    if (!parse_result) {
      return rocksdb::Status::InvalidArgument("value is not an integer or out of range");
    }
    if (isspace(value[0])) {
      return rocksdb::Status::InvalidArgument("value is not an integer");
    }
    n = *parse_result;
  }
  if ((increment < 0 && n <= 0 && increment < (LLONG_MIN - n)) ||
      (increment > 0 && n >= 0 && increment > (LLONG_MAX - n))) {
    return rocksdb::Status::InvalidArgument("increment or decrement would overflow");
  }
  n += increment;
  *new_value = n;

  raw_value = raw_value.substr(0, offset);
  raw_value.append(std::to_string(n));
  return updateRawValue(ns_key, raw_value);
}

rocksdb::Status String::IncrByFloat(const std::string &user_key, double increment, double *new_value) {
  std::string ns_key = AppendNamespacePrefix(user_key);
  LockGuard guard(storage_->GetLockManager(), ns_key);
  std::string raw_value;
  rocksdb::Status s = getRawValue(ns_key, &raw_value);
  if (!s.ok() && !s.IsNotFound()) return s;

  if (s.IsNotFound()) {
    Metadata metadata(kRedisString, false);
    metadata.Encode(&raw_value);
  }
  size_t offset = Metadata::GetOffsetAfterExpire(raw_value[0]);
  std::string value = raw_value.substr(offset);
  double n = 0;
  if (!value.empty()) {
    auto n_stat = ParseFloat(value);
    if (!n_stat || isspace(value[0])) {
      return rocksdb::Status::InvalidArgument("value is not a number");
    }
    n = *n_stat;
  }

  n += increment;
  if (std::isinf(n) || std::isnan(n)) {
    return rocksdb::Status::InvalidArgument("increment would produce NaN or Infinity");
  }
  *new_value = n;

  raw_value = raw_value.substr(0, offset);
  raw_value.append(std::to_string(n));
  return updateRawValue(ns_key, raw_value);
}

rocksdb::Status String::MSet(const std::vector<StringPair> &pairs, uint64_t ttl, bool lock) {
  uint64_t expire = 0;
  if (ttl > 0) {
    uint64_t now = util::GetTimeStampMS();
    expire = now + ttl;
  }

  // Data race, key string maybe overwrite by other key while didn't lock the keys here,
  // to improve the set performance
  std::optional<MultiLockGuard> guard;
  if (lock) {
    std::vector<std::string> lock_keys;
    lock_keys.reserve(pairs.size());
    for (const StringPair &pair : pairs) {
      std::string ns_key = AppendNamespacePrefix(pair.key);
      lock_keys.emplace_back(std::move(ns_key));
    }
    guard.emplace(storage_->GetLockManager(), lock_keys);
  }

  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisString);
  batch->PutLogData(log_data.Encode());
  for (const auto &pair : pairs) {
    std::string bytes;
    Metadata metadata(kRedisString, false);
    metadata.expire = expire;
    metadata.Encode(&bytes);
    bytes.append(pair.value.data(), pair.value.size());
    std::string ns_key = AppendNamespacePrefix(pair.key);
    batch->Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

rocksdb::Status String::MSetNX(const std::vector<StringPair> &pairs, uint64_t ttl, bool *flag) {
  *flag = false;

  int exists = 0;
  std::vector<std::string> lock_keys;
  lock_keys.reserve(pairs.size());
  std::vector<Slice> keys;
  keys.reserve(pairs.size());

  for (StringPair pair : pairs) {
    std::string ns_key = AppendNamespacePrefix(pair.key);
    lock_keys.emplace_back(std::move(ns_key));
    keys.emplace_back(pair.key);
  }

  // Lock these keys before doing anything.
  MultiLockGuard guard(storage_->GetLockManager(), lock_keys);

  if (Exists(keys, &exists).ok() && exists > 0) {
    return rocksdb::Status::OK();
  }

  rocksdb::Status s = MSet(pairs, /*ttl=*/ttl, /*lock=*/false);
  if (!s.ok()) return s;

  *flag = true;
  return rocksdb::Status::OK();
}

// Change the value of user_key to a new_value if the current value of the key matches old_value.
// ret will be:
//  1 if the operation is successful
//  -1 if the user_key does not exist
//  0 if the operation fails
rocksdb::Status String::CAS(const std::string &user_key, const std::string &old_value, const std::string &new_value,
                            uint64_t ttl, int *flag) {
  *flag = 0;

  std::string current_value;
  std::string ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  rocksdb::Status s = getValue(ns_key, &current_value);

  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }

  if (s.IsNotFound()) {
    *flag = -1;
    return rocksdb::Status::OK();
  }

  if (old_value == current_value) {
    std::string raw_value;
    uint64_t expire = 0;
    Metadata metadata(kRedisString, false);
    if (ttl > 0) {
      uint64_t now = util::GetTimeStampMS();
      expire = now + ttl;
    }
    metadata.expire = expire;
    metadata.Encode(&raw_value);
    raw_value.append(new_value);
    auto write_status = updateRawValue(ns_key, raw_value);
    if (!write_status.ok()) {
      return write_status;
    }
    *flag = 1;
  }

  return rocksdb::Status::OK();
}

// Delete a specified user_key if the current value of the user_key matches a specified value.
// For ret, same as CAS.
rocksdb::Status String::CAD(const std::string &user_key, const std::string &value, int *flag) {
  *flag = 0;

  std::string current_value;
  std::string ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  rocksdb::Status s = getValue(ns_key, &current_value);

  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }

  if (s.IsNotFound()) {
    *flag = -1;
    return rocksdb::Status::OK();
  }

  if (value == current_value) {
    auto delete_status = storage_->Delete(storage_->DefaultWriteOptions(),
                                          storage_->GetCFHandle(engine::kMetadataColumnFamilyName), ns_key);
    if (!delete_status.ok()) {
      return delete_status;
    }
    *flag = 1;
  }

  return rocksdb::Status::OK();
}

}  // namespace redis
