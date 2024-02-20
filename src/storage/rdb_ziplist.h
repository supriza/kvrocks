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

#pragma once

#include <map>
#include <string_view>

#include "common/status.h"

class ZipList {
 public:
  explicit ZipList(std::string_view input) : input_(input){};
  ~ZipList() = default;

  StatusOr<std::string> Next();
  StatusOr<std::vector<std::string>> Entries();

 private:
  std::string_view input_;
  uint64_t pos_ = 0;
  uint32_t pre_entry_len_ = 0;

  Status peekOK(size_t n);
  void setPreEntryLen(uint32_t len) { pre_entry_len_ = len; }
  static uint32_t getEncodedLengthSize(uint32_t len);
};
