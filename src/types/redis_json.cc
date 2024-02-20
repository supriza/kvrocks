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

#include "redis_json.h"

#include "json.h"
#include "lock_manager.h"
#include "storage/redis_metadata.h"

namespace redis {

rocksdb::Status Json::write(Slice ns_key, JsonMetadata *metadata, const JsonValue &json_val) {
  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisJson);
  batch->PutLogData(log_data.Encode());

  auto format = storage_->GetConfig()->json_storage_format;
  metadata->format = format;

  std::string val;
  metadata->Encode(&val);

  Status s;
  if (format == JsonStorageFormat::JSON) {
    s = json_val.Dump(&val, storage_->GetConfig()->json_max_nesting_depth);
  } else if (format == JsonStorageFormat::CBOR) {
    s = json_val.DumpCBOR(&val, storage_->GetConfig()->json_max_nesting_depth);
  } else {
    return rocksdb::Status::InvalidArgument("JSON storage format not supported");
  }
  if (!s) {
    return rocksdb::Status::InvalidArgument("Failed to encode JSON into storage: " + s.Msg());
  }

  batch->Put(metadata_cf_handle_, ns_key, val);

  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

rocksdb::Status Json::parse(const JsonMetadata &metadata, const Slice &json_bytes, JsonValue *value) {
  if (metadata.format == JsonStorageFormat::JSON) {
    auto res = JsonValue::FromString(json_bytes.ToStringView());
    if (!res) return rocksdb::Status::Corruption(res.Msg());
    *value = *std::move(res);
  } else if (metadata.format == JsonStorageFormat::CBOR) {
    auto res = JsonValue::FromCBOR(json_bytes.ToStringView());
    if (!res) return rocksdb::Status::Corruption(res.Msg());
    *value = *std::move(res);
  } else {
    return rocksdb::Status::NotSupported("JSON storage format not supported");
  }

  return rocksdb::Status::OK();
}

rocksdb::Status Json::read(const Slice &ns_key, JsonMetadata *metadata, JsonValue *value) {
  std::string bytes;
  Slice rest;

  auto s = GetMetadata({kRedisJson}, ns_key, &bytes, metadata, &rest);
  if (!s.ok()) return s;

  return parse(*metadata, rest, value);
}

rocksdb::Status Json::create(const std::string &ns_key, JsonMetadata &metadata, const std::string &value) {
  auto json_res = JsonValue::FromString(value, storage_->GetConfig()->json_max_nesting_depth);
  if (!json_res) return rocksdb::Status::InvalidArgument(json_res.Msg());
  auto json_val = *std::move(json_res);

  return write(ns_key, &metadata, json_val);
}

rocksdb::Status Json::del(const Slice &ns_key) {
  auto batch = storage_->GetWriteBatchBase();
  WriteBatchLogData log_data(kRedisJson);
  batch->PutLogData(log_data.Encode());

  batch->Delete(metadata_cf_handle_, ns_key);

  return storage_->Write(storage_->DefaultWriteOptions(), batch->GetWriteBatch());
}

rocksdb::Status Json::Info(const std::string &user_key, JsonStorageFormat *storage_format) {
  auto ns_key = AppendNamespacePrefix(user_key);

  std::string bytes;
  Slice rest;
  JsonMetadata metadata;

  auto s = GetMetadata({kRedisJson}, ns_key, &bytes, &metadata, &rest);
  if (!s.ok()) return s;

  *storage_format = metadata.format;

  return rocksdb::Status::OK();
}

rocksdb::Status Json::Set(const std::string &user_key, const std::string &path, const std::string &value) {
  auto ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);

  JsonMetadata metadata;
  JsonValue origin;
  auto s = read(ns_key, &metadata, &origin);

  if (s.IsNotFound()) {
    if (path != "$") return rocksdb::Status::InvalidArgument("new objects must be created at the root");

    return create(ns_key, metadata, value);
  }

  if (!s.ok()) return s;

  auto new_res = JsonValue::FromString(value, storage_->GetConfig()->json_max_nesting_depth);
  if (!new_res) return rocksdb::Status::InvalidArgument(new_res.Msg());
  auto new_val = *std::move(new_res);

  auto set_res = origin.Set(path, std::move(new_val));
  if (!set_res) return rocksdb::Status::InvalidArgument(set_res.Msg());

  return write(ns_key, &metadata, origin);
}

rocksdb::Status Json::Get(const std::string &user_key, const std::vector<std::string> &paths, JsonValue *result) {
  auto ns_key = AppendNamespacePrefix(user_key);

  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;

  JsonValue res;

  if (paths.empty()) {
    res = std::move(json_val);
  } else if (paths.size() == 1) {
    auto get_res = json_val.Get(paths[0]);
    if (!get_res) return rocksdb::Status::InvalidArgument(get_res.Msg());
    res = *std::move(get_res);
  } else {
    for (const auto &path : paths) {
      auto get_res = json_val.Get(path);
      if (!get_res) return rocksdb::Status::InvalidArgument(get_res.Msg());
      res.value.insert_or_assign(path, std::move(get_res->value));
    }
  }

  *result = std::move(res);
  return rocksdb::Status::OK();
}

rocksdb::Status Json::ArrAppend(const std::string &user_key, const std::string &path,
                                const std::vector<std::string> &values, Optionals<size_t> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);

  std::vector<jsoncons::json> append_values;
  append_values.reserve(values.size());
  for (auto &v : values) {
    auto value_res = JsonValue::FromString(v, storage_->GetConfig()->json_max_nesting_depth);
    if (!value_res) return rocksdb::Status::InvalidArgument(value_res.Msg());
    auto value = *std::move(value_res);
    append_values.emplace_back(std::move(value.value));
  }

  LockGuard guard(storage_->GetLockManager(), ns_key);

  JsonMetadata metadata;
  JsonValue value;
  auto s = read(ns_key, &metadata, &value);
  if (!s.ok()) return s;

  auto append_res = value.ArrAppend(path, append_values);
  if (!append_res) return rocksdb::Status::InvalidArgument(append_res.Msg());
  *results = std::move(*append_res);

  bool is_write =
      std::any_of(results->begin(), results->end(), [](std::optional<uint64_t> c) { return c.has_value(); });
  if (!is_write) return rocksdb::Status::OK();

  return write(ns_key, &metadata, value);
}

rocksdb::Status Json::ArrIndex(const std::string &user_key, const std::string &path, const std::string &needle,
                               ssize_t start, ssize_t end, Optionals<ssize_t> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);

  auto needle_res = JsonValue::FromString(needle, storage_->GetConfig()->json_max_nesting_depth);
  if (!needle_res) return rocksdb::Status::InvalidArgument(needle_res.Msg());
  auto needle_value = *std::move(needle_res);

  JsonMetadata metadata;
  JsonValue value;
  auto s = read(ns_key, &metadata, &value);
  if (!s.ok()) return s;

  auto index_res = value.ArrIndex(path, needle_value.value, start, end);
  if (!index_res) return rocksdb::Status::InvalidArgument(index_res.Msg());

  *results = std::move(*index_res);
  return rocksdb::Status::OK();
}

rocksdb::Status Json::Type(const std::string &user_key, const std::string &path, std::vector<std::string> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);

  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;

  auto res = json_val.Type(path);
  if (!res) return rocksdb::Status::InvalidArgument(res.Msg());

  *results = *res;
  return rocksdb::Status::OK();
}

rocksdb::Status Json::Merge(const std::string &user_key, const std::string &path, const std::string &merge_value,
                            bool &result) {
  auto ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);

  JsonMetadata metadata;
  JsonValue json_val;

  auto s = read(ns_key, &metadata, &json_val);

  if (s.IsNotFound()) {
    if (path != "$") return rocksdb::Status::InvalidArgument("new objects must be created at the root");
    result = true;
    return create(ns_key, metadata, merge_value);
  }

  if (!s.ok()) return s;

  auto res = json_val.Merge(path, merge_value);

  if (!res.IsOK()) return s;

  result = static_cast<bool>(res.GetValue());
  if (!res) {
    return rocksdb::Status::OK();
  }

  return write(ns_key, &metadata, json_val);
}

rocksdb::Status Json::Clear(const std::string &user_key, const std::string &path, size_t *result) {
  auto ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);

  JsonValue json_val;
  JsonMetadata metadata;
  auto s = read(ns_key, &metadata, &json_val);

  if (!s.ok()) return s;

  auto res = json_val.Clear(path);
  if (!res) return rocksdb::Status::InvalidArgument(res.Msg());

  *result = *res;
  if (*result == 0) {
    return rocksdb::Status::OK();
  }

  return write(ns_key, &metadata, json_val);
}

rocksdb::Status Json::ArrLen(const std::string &user_key, const std::string &path, Optionals<uint64_t> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);
  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;

  auto len_res = json_val.ArrLen(path);
  if (!len_res) return rocksdb::Status::InvalidArgument(len_res.Msg());

  *results = std::move(*len_res);
  return rocksdb::Status::OK();
}

rocksdb::Status Json::ArrInsert(const std::string &user_key, const std::string &path, const int64_t &index,
                                const std::vector<std::string> &values, Optionals<uint64_t> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);

  std::vector<jsoncons::json> insert_values;
  insert_values.reserve(values.size());
  for (auto &v : values) {
    auto value_res = JsonValue::FromString(v, storage_->GetConfig()->json_max_nesting_depth);
    if (!value_res) return rocksdb::Status::InvalidArgument(value_res.Msg());
    auto value = *std::move(value_res);
    insert_values.emplace_back(std::move(value.value));
  }

  LockGuard guard(storage_->GetLockManager(), ns_key);

  JsonMetadata metadata;
  JsonValue value;
  auto s = read(ns_key, &metadata, &value);
  if (!s.ok()) return s;

  auto insert_res = value.ArrInsert(path, index, insert_values);
  if (!insert_res) return rocksdb::Status::InvalidArgument(insert_res.Msg());
  *results = std::move(*insert_res);

  bool is_write =
      std::any_of(results->begin(), results->end(), [](std::optional<uint64_t> c) { return c.has_value(); });
  if (!is_write) return rocksdb::Status::OK();

  return write(ns_key, &metadata, value);
}

rocksdb::Status Json::Toggle(const std::string &user_key, const std::string &path, Optionals<bool> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);

  JsonMetadata metadata;
  JsonValue origin;
  auto s = read(ns_key, &metadata, &origin);
  if (!s.ok()) return s;

  auto toggle_res = origin.Toggle(path);
  if (!toggle_res) return rocksdb::Status::InvalidArgument(toggle_res.Msg());
  *results = std::move(*toggle_res);

  return write(ns_key, &metadata, origin);
}

rocksdb::Status Json::ArrPop(const std::string &user_key, const std::string &path, int64_t index,
                             std::vector<std::optional<JsonValue>> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);

  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;

  auto pop_res = json_val.ArrPop(path, index);
  if (!pop_res) return rocksdb::Status::InvalidArgument(pop_res.Msg());
  *results = *pop_res;

  bool is_write = std::any_of(pop_res->begin(), pop_res->end(),
                              [](const std::optional<JsonValue> &val) { return val.has_value(); });
  if (!is_write) return rocksdb::Status::OK();

  return write(ns_key, &metadata, json_val);
}

rocksdb::Status Json::ObjKeys(const std::string &user_key, const std::string &path,
                              Optionals<std::vector<std::string>> *keys) {
  auto ns_key = AppendNamespacePrefix(user_key);
  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;
  auto keys_res = json_val.ObjKeys(path);
  if (!keys_res) return rocksdb::Status::InvalidArgument(keys_res.Msg());

  *keys = std::move(*keys_res);
  return rocksdb::Status::OK();
}

rocksdb::Status Json::ArrTrim(const std::string &user_key, const std::string &path, int64_t start, int64_t stop,
                              Optionals<uint64_t> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);

  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;

  auto len_res = json_val.ArrTrim(path, start, stop);
  if (!len_res) return rocksdb::Status::InvalidArgument(len_res.Msg());

  *results = std::move(*len_res);
  bool is_write =
      std::any_of(results->begin(), results->end(), [](const std::optional<uint64_t> &val) { return val.has_value(); });
  if (!is_write) return rocksdb::Status::OK();
  return write(ns_key, &metadata, json_val);
}

rocksdb::Status Json::Del(const std::string &user_key, const std::string &path, size_t *result) {
  *result = 0;

  auto ns_key = AppendNamespacePrefix(user_key);
  LockGuard guard(storage_->GetLockManager(), ns_key);
  JsonValue json_val;
  JsonMetadata metadata;
  auto s = read(ns_key, &metadata, &json_val);

  if (!s.ok() && !s.IsNotFound()) return s;
  if (s.IsNotFound()) {
    return rocksdb::Status::OK();
  }

  if (path == "$") {
    *result = 1;
    return del(ns_key);
  }

  auto res = json_val.Del(path);
  if (!res) return rocksdb::Status::InvalidArgument(res.Msg());

  *result = *res;
  if (*result == 0) {
    return rocksdb::Status::OK();
  }
  return write(ns_key, &metadata, json_val);
}

rocksdb::Status Json::NumIncrBy(const std::string &user_key, const std::string &path, const std::string &value,
                                JsonValue *result) {
  return numop(JsonValue::NumOpEnum::Incr, user_key, path, value, result);
}

rocksdb::Status Json::NumMultBy(const std::string &user_key, const std::string &path, const std::string &value,
                                JsonValue *result) {
  return numop(JsonValue::NumOpEnum::Mul, user_key, path, value, result);
}

rocksdb::Status Json::numop(JsonValue::NumOpEnum op, const std::string &user_key, const std::string &path,
                            const std::string &value, JsonValue *result) {
  JsonValue number;
  auto number_res = JsonValue::FromString(value);
  if (!number_res || !number_res.GetValue().value.is_number()) {
    return rocksdb::Status::InvalidArgument("should be a number");
  }
  number = std::move(number_res.GetValue());

  auto ns_key = AppendNamespacePrefix(user_key);
  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;

  LockGuard guard(storage_->GetLockManager(), ns_key);

  auto res = json_val.NumOp(path, number, op, result);
  if (!res) {
    return rocksdb::Status::InvalidArgument(res.Msg());
  }
  return write(ns_key, &metadata, json_val);
}

rocksdb::Status Json::StrAppend(const std::string &user_key, const std::string &path, const std::string &value,
                                Optionals<uint64_t> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);
  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;

  auto append_res = json_val.StrAppend(path, value);
  if (!append_res) return rocksdb::Status::InvalidArgument(append_res.Msg());
  *results = std::move(*append_res);

  bool need_overwrite =
      std::any_of(results->begin(), results->end(), [](const std::optional<uint64_t> &val) { return val.has_value(); });
  if (!need_overwrite) {
    return rocksdb::Status::OK();
  }

  return write(ns_key, &metadata, json_val);
}

rocksdb::Status Json::StrLen(const std::string &user_key, const std::string &path, Optionals<uint64_t> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);
  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;

  auto str_lens = json_val.StrLen(path);
  if (!str_lens) return rocksdb::Status::InvalidArgument(str_lens.Msg());
  *results = std::move(*str_lens);
  return rocksdb::Status::OK();
}

rocksdb::Status Json::ObjLen(const std::string &user_key, const std::string &path, Optionals<uint64_t> *results) {
  auto ns_key = AppendNamespacePrefix(user_key);
  JsonMetadata metadata;
  JsonValue json_val;
  auto s = read(ns_key, &metadata, &json_val);
  if (!s.ok()) return s;

  auto obj_lens = json_val.ObjLen(path);
  if (!obj_lens) return rocksdb::Status::InvalidArgument(obj_lens.Msg());
  *results = std::move(*obj_lens);
  return rocksdb::Status::OK();
}

std::vector<rocksdb::Status> Json::MGet(const std::vector<std::string> &user_keys, const std::string &path,
                                        std::vector<JsonValue> &results) {
  std::vector<Slice> ns_keys;
  std::vector<std::string> ns_keys_string;
  ns_keys.resize(user_keys.size());
  ns_keys_string.resize(user_keys.size());

  for (size_t i = 0; i < user_keys.size(); i++) {
    ns_keys_string[i] = AppendNamespacePrefix(user_keys[i]);
    ns_keys[i] = Slice(ns_keys_string[i]);
  }

  std::vector<JsonValue> json_vals;
  json_vals.resize(ns_keys.size());
  auto statuses = readMulti(ns_keys, json_vals);

  results.resize(ns_keys.size());
  for (size_t i = 0; i < ns_keys.size(); i++) {
    if (!statuses[i].ok()) {
      continue;
    }
    auto res = json_vals[i].Get(path);

    if (!res) {
      statuses[i] = rocksdb::Status::Corruption(res.Msg());
    } else {
      results[i] = *std::move(res);
    }
  }
  return statuses;
}

std::vector<rocksdb::Status> Json::readMulti(const std::vector<Slice> &ns_keys, std::vector<JsonValue> &values) {
  rocksdb::ReadOptions read_options = storage_->DefaultMultiGetOptions();
  LatestSnapShot ss(storage_);
  read_options.snapshot = ss.GetSnapShot();

  std::vector<rocksdb::Status> statuses(ns_keys.size());
  std::vector<rocksdb::PinnableSlice> pin_values(ns_keys.size());
  storage_->MultiGet(read_options, metadata_cf_handle_, ns_keys.size(), ns_keys.data(), pin_values.data(),
                     statuses.data());
  for (size_t i = 0; i < ns_keys.size(); i++) {
    if (!statuses[i].ok()) continue;
    Slice rest(pin_values[i].data(), pin_values[i].size());
    JsonMetadata metadata;
    statuses[i] = ParseMetadata({kRedisJson}, &rest, &metadata);
    if (!statuses[i].ok()) continue;

    statuses[i] = parse(metadata, rest, &values[i]);
    if (!statuses[i].ok()) continue;
  }
  return statuses;
}

}  // namespace redis
