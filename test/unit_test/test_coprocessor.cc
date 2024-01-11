// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <dirent.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "butil/status.h"
#include "common/helper.h"
#include "config/config.h"
#include "config/yaml_config.h"
#include "coprocessor/coprocessor.h"
#include "engine/rocks_raw_engine.h"
#include "proto/common.pb.h"
#include "proto/error.pb.h"
#include "proto/store_internal.pb.h"
#include "serial/record_encoder.h"
#include "serial/schema/base_schema.h"
#include "serial/schema/boolean_schema.h"
#include "serial/schema/double_schema.h"
#include "serial/schema/float_schema.h"
#include "serial/schema/integer_schema.h"
#include "serial/schema/long_schema.h"
#include "serial/schema/string_schema.h"

namespace dingodb {

static const std::string kDefaultCf = "default";

static const std::vector<std::string> kAllCFs = {kDefaultCf};

const char kAlphabet[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
                          's', 't', 'o', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

const std::string kRootPath = "./unit_test";
const std::string kLogPath = kRootPath + "/log";
const std::string kStorePath = kRootPath + "/db";

const std::string kYamlConfigContent =
    "cluster:\n"
    "  name: dingodb\n"
    "  instance_id: 12345\n"
    "  coordinators: 127.0.0.1:19190,127.0.0.1:19191,127.0.0.1:19192\n"
    "  keyring: TO_BE_CONTINUED\n"
    "server:\n"
    "  host: 127.0.0.1\n"
    "  port: 23000\n"
    "log:\n"
    "  path: " +
    kLogPath +
    "\n"
    "store:\n"
    "  path: " +
    kStorePath + "\n";

static std::string StrToHex(std::string str, std::string separator = "") {
  const std::string hex = "0123456789ABCDEF";
  std::stringstream ss;

  for (char i : str) ss << hex[(unsigned char)i >> 4] << hex[(unsigned char)i & 0xf] << separator;

  return ss.str();
}

// rand string
static std::string GenRandomString(int len) {
  std::string result;
  int alphabet_len = sizeof(kAlphabet);

  std::mt19937 rng;
  rng.seed(std::random_device()());
  std::uniform_int_distribution<std::mt19937::result_type> distrib(1, 1000000000);
  for (int i = 0; i < len; ++i) {
    result.append(1, kAlphabet[distrib(rng) % alphabet_len]);
  }

  return result;
}

class CoprocessorTest : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    dingodb::Helper::CreateDirectories(kStorePath);
    std::srand(std::time(nullptr));

    std::shared_ptr<Config> config = std::make_shared<YamlConfig>();
    if (config->Load(kYamlConfigContent) != 0) {
      std::cout << "Load config failed" << '\n';
      return;
    }

    engine = std::make_shared<RocksRawEngine>();
    if (!engine->Init(config, kAllCFs)) {
      std::cout << "RocksRawEngine init failed" << '\n';
    }

    coprocessor = std::make_shared<Coprocessor>();
  }

  static void TearDownTestSuite() {
    engine->Close();
    engine->Destroy();
    dingodb::Helper::RemoveAllFileOrDirectory(kRootPath);
  }

  void SetUp() override {}

  void TearDown() override {}

  static std::shared_ptr<RocksRawEngine> engine;
  static std::shared_ptr<Coprocessor> coprocessor;

  static std::string max_key;
  static std::string min_key;

  static size_t max_min_size;
  static size_t min_min_size;
};

std::shared_ptr<RocksRawEngine> CoprocessorTest::engine = nullptr;

std::shared_ptr<Coprocessor> CoprocessorTest::coprocessor = nullptr;

std::string CoprocessorTest::max_key;
std::string CoprocessorTest::min_key;

size_t CoprocessorTest::max_min_size = 0;
size_t CoprocessorTest::min_min_size = 0;

TEST_F(CoprocessorTest, Open) {
  butil::Status ok;

  // original_schema  empty
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::Errno::OK);
  }

  // selection empty failed
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::Errno::OK);
  }

  // result empty failed
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    auto *selection_columns = pb_coprocessor.mutable_selection_columns();
    selection_columns->Add(0);
    selection_columns->Add(1);
    selection_columns->Add(2);
    selection_columns->Add(3);
    selection_columns->Add(4);
    selection_columns->Add(5);
    selection_columns->Add(0);
    selection_columns->Add(1);
    selection_columns->Add(2);
    selection_columns->Add(3);
    selection_columns->Add(4);
    selection_columns->Add(5);

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::Errno::OK);
  }

  // ok but not exist aggregation
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    // auto *selection_columns = pb_coprocessor.mutable_selection_columns();
    // selection_columns->Add(0);
    // selection_columns->Add(1);
    // selection_columns->Add(2);
    // selection_columns->Add(3);
    // selection_columns->Add(4);
    // selection_columns->Add(5);
    // selection_columns->Add(0);
    // selection_columns->Add(1);
    // selection_columns->Add(2);
    // selection_columns->Add(3);
    // selection_columns->Add(4);
    // selection_columns->Add(5);

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(true);
        schema1->set_is_nullable(true);
        schema1->set_index(0);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(1);
      }

      auto *schema3 = result_schema->add_schema();
      {
        schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
        schema3->set_is_key(false);
        schema3->set_is_nullable(true);
        schema3->set_index(2);
      }

      auto *schema4 = result_schema->add_schema();
      {
        schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema4->set_is_key(false);
        schema4->set_is_nullable(true);
        schema4->set_index(3);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(4);
      }

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(5);
      }

      // auto *schema7 = result_schema->add_schema();
      // {
      //   schema7->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      //   schema7->set_is_key(true);
      //   schema7->set_is_nullable(true);
      //   schema7->set_index(6);
      // }

      // auto *schema8 = result_schema->add_schema();
      // {
      //   schema8->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      //   schema8->set_is_key(false);
      //   schema8->set_is_nullable(true);
      //   schema8->set_index(7);
      // }

      // auto *schema9 = result_schema->add_schema();
      // {
      //   schema9->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      //   schema9->set_is_key(false);
      //   schema9->set_is_nullable(true);
      //   schema9->set_index(8);
      // }

      // auto *schema10 = result_schema->add_schema();
      // {
      //   schema10->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      //   schema10->set_is_key(false);
      //   schema10->set_is_nullable(true);
      //   schema10->set_index(9);
      // }

      // auto *schema11 = result_schema->add_schema();
      // {
      //   schema11->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      //   schema11->set_is_key(true);
      //   schema11->set_is_nullable(true);
      //   schema11->set_index(10);
      // }

      // auto *schema12 = result_schema->add_schema();
      // {
      //   schema12->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      //   schema12->set_is_key(true);
      //   schema12->set_is_nullable(true);
      //   schema12->set_index(11);
      // }
    }

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }

  // ok has aggregation
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    auto *selection_columns = pb_coprocessor.mutable_selection_columns();
    selection_columns->Add(0);
    selection_columns->Add(1);
    selection_columns->Add(2);
    selection_columns->Add(3);
    selection_columns->Add(4);
    selection_columns->Add(5);
    // selection_columns->Add(0);
    // selection_columns->Add(1);
    // selection_columns->Add(2);
    // selection_columns->Add(3);
    // selection_columns->Add(4);
    // selection_columns->Add(5);

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(true);
        schema1->set_is_nullable(true);
        schema1->set_index(0);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(1);
      }

      auto *schema3 = result_schema->add_schema();
      {
        schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
        schema3->set_is_key(false);
        schema3->set_is_nullable(true);
        schema3->set_index(2);
      }

      auto *schema4 = result_schema->add_schema();
      {
        schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema4->set_is_key(false);
        schema4->set_is_nullable(true);
        schema4->set_index(3);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(4);
      }

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(5);
      }

      auto *schema7 = result_schema->add_schema();
      {
        schema7->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema7->set_is_key(true);
        schema7->set_is_nullable(true);
        schema7->set_index(6);
      }

      auto *schema8 = result_schema->add_schema();
      {
        schema8->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema8->set_is_key(false);
        schema8->set_is_nullable(true);
        schema8->set_index(7);
      }

      auto *schema9 = result_schema->add_schema();
      {
        schema9->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema9->set_is_key(false);
        schema9->set_is_nullable(true);
        schema9->set_index(8);
      }

      auto *schema10 = result_schema->add_schema();
      {
        schema10->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema10->set_is_key(false);
        schema10->set_is_nullable(true);
        schema10->set_index(9);
      }

      auto *schema11 = result_schema->add_schema();
      {
        schema11->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema11->set_is_key(true);
        schema11->set_is_nullable(true);
        schema11->set_index(10);
      }

      auto *schema12 = result_schema->add_schema();
      {
        schema12->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema12->set_is_key(true);
        schema12->set_is_nullable(true);
        schema12->set_index(11);
      }
    }

    // aggression
    pb_coprocessor.add_group_by_columns(0);
    pb_coprocessor.add_group_by_columns(1);
    pb_coprocessor.add_group_by_columns(2);
    pb_coprocessor.add_group_by_columns(3);
    pb_coprocessor.add_group_by_columns(4);
    pb_coprocessor.add_group_by_columns(5);

    auto *aggregation_operator1 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator1->set_oper(::dingodb::pb::store::AggregationType::SUM);
      aggregation_operator1->set_index_of_column(0);
    }

    auto *aggregation_operator2 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator2->set_oper(::dingodb::pb::store::AggregationType::COUNT);
      aggregation_operator2->set_index_of_column(1);
    }

    auto *aggregation_operator3 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator3->set_oper(::dingodb::pb::store::AggregationType::COUNTWITHNULL);
      aggregation_operator3->set_index_of_column(88);
    }

    auto *aggregation_operator4 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator4->set_oper(::dingodb::pb::store::AggregationType::MAX);
      aggregation_operator4->set_index_of_column(3);
    }

    auto *aggregation_operator5 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator5->set_oper(::dingodb::pb::store::AggregationType::MIN);
      aggregation_operator5->set_index_of_column(4);
    }

    auto *aggregation_operator6 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator6->set_oper(::dingodb::pb::store::AggregationType::COUNT);
      aggregation_operator6->set_index_of_column(-1);
    }

    coprocessor.reset();
    coprocessor = std::make_shared<Coprocessor>();

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }
}

TEST_F(CoprocessorTest, Prepare) {
  int schema_version = 1;
  std::shared_ptr<std::vector<std::shared_ptr<BaseSchema>>> schemas;
  long common_id = 1;  // NOLINT

  schemas = std::make_shared<std::vector<std::shared_ptr<BaseSchema>>>();

  schemas->reserve(6);

  std::shared_ptr<DingoSchema<std::optional<bool>>> bool_schema = std::make_shared<DingoSchema<std::optional<bool>>>();
  bool_schema->SetIsKey(true);
  bool_schema->SetAllowNull(true);
  bool_schema->SetIndex(0);

  schemas->emplace_back(std::move(bool_schema));

  std::shared_ptr<DingoSchema<std::optional<int32_t>>> int_schema =
      std::make_shared<DingoSchema<std::optional<int32_t>>>();
  int_schema->SetIsKey(false);
  int_schema->SetAllowNull(true);
  int_schema->SetIndex(1);
  schemas->emplace_back(std::move(int_schema));

  std::shared_ptr<DingoSchema<std::optional<float>>> float_schema =
      std::make_shared<DingoSchema<std::optional<float>>>();
  float_schema->SetIsKey(false);
  float_schema->SetAllowNull(true);
  float_schema->SetIndex(2);
  schemas->emplace_back(std::move(float_schema));

  std::shared_ptr<DingoSchema<std::optional<int64_t>>> long_schema =
      std::make_shared<DingoSchema<std::optional<int64_t>>>();
  long_schema->SetIsKey(false);
  long_schema->SetAllowNull(true);
  long_schema->SetIndex(3);
  schemas->emplace_back(std::move(long_schema));

  std::shared_ptr<DingoSchema<std::optional<double>>> double_schema =
      std::make_shared<DingoSchema<std::optional<double>>>();
  double_schema->SetIsKey(true);
  double_schema->SetAllowNull(true);
  double_schema->SetIndex(4);
  schemas->emplace_back(std::move(double_schema));

  std::shared_ptr<DingoSchema<std::optional<std::shared_ptr<std::string>>>> string_schema =
      std::make_shared<DingoSchema<std::optional<std::shared_ptr<std::string>>>>();
  string_schema->SetIsKey(true);
  string_schema->SetAllowNull(true);
  string_schema->SetIndex(5);
  schemas->emplace_back(std::move(string_schema));

  RecordEncoder record_encoder(schema_version, schemas, common_id);

  // std::string min_key;
  // std::string max_key;

  // 1
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);
    std::any any_bool = std::optional<bool>(std::nullopt);
    record.emplace_back(std::move(any_bool));

    std::any any_int = std::optional<int32_t>(std::nullopt);
    record.emplace_back(std::move(any_int));

    std::any any_float = std::optional<float>(std::nullopt);
    record.emplace_back(std::move(any_float));

    std::any any_long = std::optional<int64_t>(std::nullopt);
    record.emplace_back(std::move(any_long));

    std::any any_double = std::optional<double>(std::nullopt);
    record.emplace_back(std::move(any_double));

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::nullopt);
    record.emplace_back(std::move(any_string));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 2
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);
    std::any any_bool = std::optional<bool>(false);
    record.emplace_back(std::move(any_bool));

    std::any any_int = std::optional<int32_t>(1);
    record.emplace_back(std::move(any_int));

    std::any any_float = std::optional<float>(1.23);
    record.emplace_back(std::move(any_float));

    std::any any_long = std::optional<int64_t>(100);
    record.emplace_back(std::move(any_long));

    std::any any_double = std::optional<double>(23.4545);
    record.emplace_back(std::move(any_double));

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("fdf45nrthn"));
    record.emplace_back(std::move(any_string));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 3
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);
    std::any any_bool = std::optional<bool>(true);
    record.emplace_back(std::move(any_bool));

    std::any any_int = std::optional<int32_t>(2);
    record.emplace_back(std::move(any_int));

    std::any any_float = std::optional<float>(2.23);
    record.emplace_back(std::move(any_float));

    std::any any_long = std::optional<int64_t>(200);
    record.emplace_back(std::move(any_long));

    std::any any_double = std::optional<double>(3443.5656);
    record.emplace_back(std::move(any_double));

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("sssfdf45nrthn"));
    record.emplace_back(std::move(any_string));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 4
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);
    std::any any_bool = std::optional<bool>(3);
    record.emplace_back(std::move(any_bool));

    std::any any_int = std::optional<int32_t>(std::nullopt);
    record.emplace_back(std::move(any_int));

    std::any any_float = std::optional<float>(3.23);
    record.emplace_back(std::move(any_float));

    std::any any_long = std::optional<int64_t>(232545);
    record.emplace_back(std::move(any_long));

    std::any any_double = std::optional<double>(3434343443.56565);
    record.emplace_back(std::move(any_double));

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("cccfdf45nrthn"));
    record.emplace_back(std::move(any_string));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 5
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);
    std::any any_bool = std::optional<bool>(true);
    record.emplace_back(std::move(any_bool));

    std::any any_int = std::optional<int32_t>(4);
    record.emplace_back(std::move(any_int));

    std::any any_float = std::optional<float>(4.23);
    record.emplace_back(std::move(any_float));

    std::any any_long = std::optional<int64_t>(std::nullopt);
    record.emplace_back(std::move(any_long));

    std::any any_double = std::optional<double>(std::nullopt);
    record.emplace_back(std::move(any_double));

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("errerfdf45nrthn"));
    record.emplace_back(std::move(any_string));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 6
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);
    std::any any_bool = std::optional<bool>(5);
    record.emplace_back(std::move(any_bool));

    std::any any_int = std::optional<int32_t>(std::nullopt);
    record.emplace_back(std::move(any_int));

    std::any any_float = std::optional<float>(5.23);
    record.emplace_back(std::move(any_float));

    std::any any_long = std::optional<int64_t>(123455666);
    record.emplace_back(std::move(any_long));

    std::any any_double = std::optional<double>(99888343434);
    record.emplace_back(std::move(any_double));

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::nullopt);
    record.emplace_back(std::move(any_string));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    size_t min_size = std::min(key.size(), min_key.size());
    if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0 || min_key.empty()) {
      min_key = key;
    }

    min_size = std::min(key.size(), max_key.size());
    if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0 || max_key.empty()) {
      max_key = key;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 7
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);
    std::any any_bool = std::optional<bool>(false);
    record.emplace_back(std::move(any_bool));

    std::any any_int = std::optional<int32_t>(6);
    record.emplace_back(std::move(any_int));

    std::any any_float = std::optional<float>(6.23);
    record.emplace_back(std::move(any_float));

    std::any any_long = std::optional<int64_t>(11111111);
    record.emplace_back(std::move(any_long));

    std::any any_double = std::optional<double>(0.123232323);
    record.emplace_back(std::move(any_double));

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("dfaerj56j"));
    record.emplace_back(std::move(any_string));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 8
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);
    std::any any_bool = std::optional<bool>(true);
    record.emplace_back(std::move(any_bool));

    std::any any_int = std::optional<int32_t>(7);
    record.emplace_back(std::move(any_int));

    std::any any_float = std::optional<float>(7.23);
    record.emplace_back(std::move(any_float));

    std::any any_long = std::optional<int64_t>(1111111111111);
    record.emplace_back(std::move(any_long));

    std::any any_double = std::optional<double>(454.343434);
    record.emplace_back(std::move(any_double));

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::nullopt);
    record.emplace_back(std::move(any_string));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }
}

TEST_F(CoprocessorTest, Execute) {
  butil::Status ok;
  // std::string my_min_key;
  // char my_max_key_char[] = {static_cast<char>(0xEF), static_cast<char>(0xEF), static_cast<char>(0xEF)};
  // std::string my_max_key(my_max_key_char, sizeof(my_max_key_char));

  std::string my_min_key(min_key.c_str(), 8);
  std::string my_max_key(max_key.c_str(), 8);

  std::string my_min_key_s = StrToHex(my_min_key, " ");
  std::cout << "my_min_key_s : " << my_min_key_s << '\n';

  std::string my_max_key_s = StrToHex(my_max_key, " ");
  std::cout << "my_max_key_s : " << my_max_key_s << '\n';

  IteratorOptions options;
  options.upper_bound = Helper::PrefixNext(my_max_key);
  auto iter = engine->Reader()->NewIterator(kDefaultCf, options);
  bool key_only = false;
  size_t max_fetch_cnt = 2;
  int64_t max_bytes_rpc = 1000000000000000;
  std::vector<pb::common::KeyValue> kvs;

  iter->Seek(my_min_key);

  size_t cnt = 0;

  while (true) {
    bool has_more = true;
    ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
    EXPECT_EQ(ok.error_code(), pb::error::OK);
    cnt += kvs.size();
    if (kvs.empty()) {
      break;
    }
    kvs.clear();
  }
  std::cout << "key_values aggregation cnt : " << cnt << '\n';
}

// without Aggregation only selection
TEST_F(CoprocessorTest, OpenSelection) {
  butil::Status ok;

  // ok no aggregation
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    // auto *selection_columns = pb_coprocessor.mutable_selection_columns();
    // selection_columns->Add(0);
    // selection_columns->Add(1);
    // selection_columns->Add(2);
    // selection_columns->Add(3);
    // selection_columns->Add(4);
    // selection_columns->Add(5);
    // selection_columns->Add(0);
    // selection_columns->Add(1);
    // selection_columns->Add(2);
    // selection_columns->Add(3);
    // selection_columns->Add(4);
    // selection_columns->Add(5);

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(true);
        schema1->set_is_nullable(true);
        schema1->set_index(0);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(1);
      }

      auto *schema3 = result_schema->add_schema();
      {
        schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
        schema3->set_is_key(false);
        schema3->set_is_nullable(true);
        schema3->set_index(2);
      }

      auto *schema4 = result_schema->add_schema();
      {
        schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema4->set_is_key(false);
        schema4->set_is_nullable(true);
        schema4->set_index(3);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(4);
      }

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(5);
      }

      // auto *schema7 = result_schema->add_schema();
      // {
      //   schema7->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      //   schema7->set_is_key(true);
      //   schema7->set_is_nullable(true);
      //   schema7->set_index(6);
      // }

      // auto *schema8 = result_schema->add_schema();
      // {
      //   schema8->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      //   schema8->set_is_key(false);
      //   schema8->set_is_nullable(true);
      //   schema8->set_index(7);
      // }

      // auto *schema9 = result_schema->add_schema();
      // {
      //   schema9->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      //   schema9->set_is_key(false);
      //   schema9->set_is_nullable(true);
      //   schema9->set_index(8);
      // }

      // auto *schema10 = result_schema->add_schema();
      // {
      //   schema10->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      //   schema10->set_is_key(false);
      //   schema10->set_is_nullable(true);
      //   schema10->set_index(9);
      // }

      // auto *schema11 = result_schema->add_schema();
      // {
      //   schema11->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      //   schema11->set_is_key(true);
      //   schema11->set_is_nullable(true);
      //   schema11->set_index(10);
      // }

      // auto *schema12 = result_schema->add_schema();
      // {
      //   schema12->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      //   schema12->set_is_key(true);
      //   schema12->set_is_nullable(true);
      //   schema12->set_index(11);
      // }
    }
    coprocessor->Close();

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }
}

TEST_F(CoprocessorTest, ExecuteSelection) {
  butil::Status ok;

  std::string my_min_key(min_key.c_str(), 8);
  std::string my_max_key(max_key.c_str(), 8);

  std::string my_min_key_s = StrToHex(my_min_key, " ");
  std::cout << "my_min_key_s : " << my_min_key_s << '\n';

  std::string my_max_key_s = StrToHex(my_max_key, " ");
  std::cout << "my_max_key_s : " << my_max_key_s << '\n';

  IteratorOptions options;
  options.upper_bound = Helper::PrefixNext(my_max_key);
  auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

  bool key_only = false;
  size_t max_fetch_cnt = 2;
  int64_t max_bytes_rpc = 1000000000000000;
  std::vector<pb::common::KeyValue> kvs;

  iter->Seek(my_min_key);

  size_t cnt = 0;

  while (true) {
    bool has_more = true;
    ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
    EXPECT_EQ(ok.error_code(), pb::error::OK);
    cnt += kvs.size();
    if (kvs.empty()) {
      break;
    }
    kvs.clear();
  }
  std::cout << "key_values selection cnt : " << cnt << '\n';
}

// without Aggregation Key
TEST_F(CoprocessorTest, OpenNoAggregationKey) {
  butil::Status ok;

  // ok has aggregation bu no  aggregation key
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    // auto *selection_columns = pb_coprocessor.mutable_selection_columns();
    // selection_columns->Add(0);
    // selection_columns->Add(1);
    // selection_columns->Add(2);
    // selection_columns->Add(3);
    // selection_columns->Add(4);
    // selection_columns->Add(5);
    // selection_columns->Add(0);
    // selection_columns->Add(1);
    // selection_columns->Add(2);
    // selection_columns->Add(3);
    // selection_columns->Add(4);
    // selection_columns->Add(5);

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema7 = result_schema->add_schema();
      {
        schema7->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema7->set_is_key(true);
        schema7->set_is_nullable(true);
        schema7->set_index(0);
      }

      auto *schema8 = result_schema->add_schema();
      {
        schema8->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema8->set_is_key(false);
        schema8->set_is_nullable(true);
        schema8->set_index(1);
      }

      auto *schema9 = result_schema->add_schema();
      {
        schema9->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema9->set_is_key(false);
        schema9->set_is_nullable(true);
        schema9->set_index(2);
      }

      auto *schema10 = result_schema->add_schema();
      {
        schema10->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema10->set_is_key(false);
        schema10->set_is_nullable(true);
        schema10->set_index(3);
      }

      auto *schema11 = result_schema->add_schema();
      {
        schema11->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema11->set_is_key(true);
        schema11->set_is_nullable(true);
        schema11->set_index(4);
      }

      auto *schema12 = result_schema->add_schema();
      {
        schema12->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema12->set_is_key(true);
        schema12->set_is_nullable(true);
        schema12->set_index(5);
      }
    }

    // aggression
    auto *aggregation_operator1 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator1->set_oper(::dingodb::pb::store::AggregationType::SUM);
      aggregation_operator1->set_index_of_column(0);
    }

    auto *aggregation_operator2 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator2->set_oper(::dingodb::pb::store::AggregationType::COUNT);
      aggregation_operator2->set_index_of_column(1);
    }

    auto *aggregation_operator3 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator3->set_oper(::dingodb::pb::store::AggregationType::COUNTWITHNULL);
      aggregation_operator3->set_index_of_column(88);
    }

    auto *aggregation_operator4 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator4->set_oper(::dingodb::pb::store::AggregationType::MAX);
      aggregation_operator4->set_index_of_column(3);
    }

    auto *aggregation_operator5 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator5->set_oper(::dingodb::pb::store::AggregationType::MIN);
      aggregation_operator5->set_index_of_column(4);
    }

    auto *aggregation_operator6 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator6->set_oper(::dingodb::pb::store::AggregationType::COUNT);
      aggregation_operator6->set_index_of_column(-1);
    }
    coprocessor->Close();
    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }
}

TEST_F(CoprocessorTest, ExecuteNoAggregationKey) {
  butil::Status ok;

  std::string my_min_key(min_key.c_str(), 8);
  std::string my_max_key(max_key.c_str(), 8);

  std::string my_min_key_s = StrToHex(my_min_key, " ");
  std::cout << "my_min_key_s : " << my_min_key_s << '\n';

  std::string my_max_key_s = StrToHex(my_max_key, " ");
  std::cout << "my_max_key_s : " << my_max_key_s << '\n';

  IteratorOptions options;
  options.upper_bound = Helper::PrefixNext(my_max_key);
  auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

  bool key_only = false;
  size_t max_fetch_cnt = 2;
  int64_t max_bytes_rpc = 1000000000000000;
  std::vector<pb::common::KeyValue> kvs;

  iter->Seek(my_min_key);

  size_t cnt = 0;

  while (true) {
    bool has_more = true;
    ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
    EXPECT_EQ(ok.error_code(), pb::error::OK);
    cnt += kvs.size();
    if (kvs.empty()) {
      break;
    }
    kvs.clear();
  }
  std::cout << "key_values no aggregation key cnt : " << cnt << '\n';
}

// without Aggregation Value
TEST_F(CoprocessorTest, OpenNoAggregationValue) {
  butil::Status ok;

  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    auto *selection_columns = pb_coprocessor.mutable_selection_columns();
    selection_columns->Add(0);
    selection_columns->Add(1);
    selection_columns->Add(2);
    selection_columns->Add(3);
    selection_columns->Add(4);
    selection_columns->Add(5);
    // selection_columns->Add(0);
    // selection_columns->Add(1);
    // selection_columns->Add(2);
    // selection_columns->Add(3);
    // selection_columns->Add(4);
    // selection_columns->Add(5);

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(true);
        schema1->set_is_nullable(true);
        schema1->set_index(0);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(1);
      }

      auto *schema3 = result_schema->add_schema();
      {
        schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
        schema3->set_is_key(false);
        schema3->set_is_nullable(true);
        schema3->set_index(2);
      }

      auto *schema4 = result_schema->add_schema();
      {
        schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema4->set_is_key(false);
        schema4->set_is_nullable(true);
        schema4->set_index(3);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(4);
      }

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(5);
      }
    }

    // aggression
    pb_coprocessor.add_group_by_columns(0);
    pb_coprocessor.add_group_by_columns(1);
    pb_coprocessor.add_group_by_columns(2);
    pb_coprocessor.add_group_by_columns(3);
    pb_coprocessor.add_group_by_columns(4);
    pb_coprocessor.add_group_by_columns(5);

    coprocessor->Close();
    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }
}

TEST_F(CoprocessorTest, ExecuteNoAggregationValue) {
  butil::Status ok;

  std::string my_min_key(min_key.c_str(), 8);
  std::string my_max_key(max_key.c_str(), 8);

  std::string my_min_key_s = StrToHex(my_min_key, " ");
  std::cout << "my_min_key_s : " << my_min_key_s << '\n';

  std::string my_max_key_s = StrToHex(my_max_key, " ");
  std::cout << "my_max_key_s : " << my_max_key_s << '\n';

  IteratorOptions options;
  options.upper_bound = Helper::PrefixNext(my_max_key);
  auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

  bool key_only = false;
  size_t max_fetch_cnt = 2;
  int64_t max_bytes_rpc = 1000000000000000;
  std::vector<pb::common::KeyValue> kvs;

  iter->Seek(my_min_key);

  size_t cnt = 0;

  while (true) {
    bool has_more = true;
    ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
    EXPECT_EQ(ok.error_code(), pb::error::OK);
    cnt += kvs.size();
    if (kvs.empty()) {
      break;
    }
    kvs.clear();
  }
  std::cout << "key_values no aggregation value cnt : " << cnt << '\n';
}

// without Aggregation only selection one
TEST_F(CoprocessorTest, OpenSelectionOne) {
  butil::Status ok;

  // ok no aggregation
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    // auto *selection_columns = pb_coprocessor.mutable_selection_columns();

    // selection_columns->Add(3);
    // selection_columns->Add(3);

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(true);
        schema1->set_is_nullable(true);
        schema1->set_index(0);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(1);
      }

      auto *schema3 = result_schema->add_schema();
      {
        schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
        schema3->set_is_key(false);
        schema3->set_is_nullable(true);
        schema3->set_index(2);
      }

      auto *schema4 = result_schema->add_schema();
      {
        schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema4->set_is_key(false);
        schema4->set_is_nullable(true);
        schema4->set_index(3);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(4);
      }

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(5);
      }
    }
    coprocessor->Close();

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }
}

TEST_F(CoprocessorTest, ExecuteSelectionOne) {
  butil::Status ok;

  std::string my_min_key(min_key.c_str(), 8);
  std::string my_max_key(max_key.c_str(), 8);

  std::string my_min_key_s = StrToHex(my_min_key, " ");
  std::cout << "my_min_key_s : " << my_min_key_s << '\n';

  std::string my_max_key_s = StrToHex(my_max_key, " ");
  std::cout << "my_max_key_s : " << my_max_key_s << '\n';

  IteratorOptions options;
  options.upper_bound = Helper::PrefixNext(my_max_key);
  auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

  bool key_only = false;
  size_t max_fetch_cnt = 2;
  int64_t max_bytes_rpc = 1000000000000000;
  std::vector<pb::common::KeyValue> kvs;

  iter->Seek(my_min_key);

  size_t cnt = 0;

  while (true) {
    bool has_more = true;
    ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
    EXPECT_EQ(ok.error_code(), pb::error::OK);
    cnt += kvs.size();
    if (kvs.empty()) {
      break;
    }
    kvs.clear();
  }
  std::cout << "key_values selection one  cnt : " << cnt << '\n';
}

// without Aggregation Key
TEST_F(CoprocessorTest, OpenNoAggregationKeyOne) {
  butil::Status ok;

  // ok has aggregation bu no  aggregation key
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    auto *selection_columns = pb_coprocessor.mutable_selection_columns();
    selection_columns->Add(3);
    // selection_columns->Add(3);

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema1->set_is_key(false);
        schema1->set_is_nullable(true);
        schema1->set_index(0);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(1);
      }
    }

    auto *aggregation_operator1 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator1->set_oper(::dingodb::pb::store::AggregationType::COUNTWITHNULL);
      aggregation_operator1->set_index_of_column(1);
    }

    auto *aggregation_operator2 = pb_coprocessor.add_aggregation_operators();
    {
      aggregation_operator2->set_oper(::dingodb::pb::store::AggregationType::COUNTWITHNULL);
      aggregation_operator2->set_index_of_column(88);
    }

    coprocessor->Close();
    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }
}

TEST_F(CoprocessorTest, ExecuteNoAggregationKeyOne) {
  butil::Status ok;

  std::string my_min_key(min_key.c_str(), 8);
  std::string my_max_key(max_key.c_str(), 8);

  std::string my_min_key_s = StrToHex(my_min_key, " ");
  std::cout << "my_min_key_s : " << my_min_key_s << '\n';

  std::string my_max_key_s = StrToHex(my_max_key, " ");
  std::cout << "my_max_key_s : " << my_max_key_s << '\n';

  IteratorOptions options;
  options.upper_bound = Helper::PrefixNext(my_max_key);
  auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

  bool key_only = false;
  size_t max_fetch_cnt = 2;
  int64_t max_bytes_rpc = 1000000000000000;
  std::vector<pb::common::KeyValue> kvs;

  iter->Seek(my_min_key);

  size_t cnt = 0;

  while (true) {
    bool has_more = true;
    ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
    EXPECT_EQ(ok.error_code(), pb::error::OK);
    cnt += kvs.size();
    if (kvs.empty()) {
      break;
    }
    kvs.clear();
  }
  std::cout << "key_values no aggregation key cnt : " << cnt << '\n';
}

// without Aggregation Value one
TEST_F(CoprocessorTest, OpenNoAggregationValueOne) {
  butil::Status ok;

  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    // auto *selection_columns = pb_coprocessor.mutable_selection_columns();
    // selection_columns->Add(3);
    // selection_columns->Add(3);

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(false);
        schema1->set_is_nullable(true);
        schema1->set_index(0);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(1);
      }
    }

    // aggression
    pb_coprocessor.add_group_by_columns(0);
    pb_coprocessor.add_group_by_columns(1);

    coprocessor->Close();
    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }
}

TEST_F(CoprocessorTest, ExecuteNoAggregationValueOne) {
  butil::Status ok;

  std::string my_min_key(min_key.c_str(), 8);
  std::string my_max_key(max_key.c_str(), 8);

  std::string my_min_key_s = StrToHex(my_min_key, " ");
  std::cout << "my_min_key_s : " << my_min_key_s << '\n';

  std::string my_max_key_s = StrToHex(my_max_key, " ");
  std::cout << "my_max_key_s : " << my_max_key_s << '\n';

  IteratorOptions options;
  options.upper_bound = Helper::PrefixNext(my_max_key);
  auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

  bool key_only = false;
  size_t max_fetch_cnt = 2;
  int64_t max_bytes_rpc = 1000000000000000;
  std::vector<pb::common::KeyValue> kvs;

  iter->Seek(my_min_key);

  size_t cnt = 0;

  while (true) {
    bool has_more = true;
    ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
    EXPECT_EQ(ok.error_code(), pb::error::OK);
    cnt += kvs.size();
    if (kvs.empty()) {
      break;
    }
    kvs.clear();
  }
  std::cout << "key_values no aggregation value cnt : " << cnt << '\n';
}

// without Aggregation Value one test empty
TEST_F(CoprocessorTest, OpenNoAggregationValueEmpty) {
  butil::Status ok;

  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    // auto *selection_columns = pb_coprocessor.mutable_selection_columns();
    // selection_columns->Add(3);
    // selection_columns->Add(3);

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(false);
        schema1->set_is_nullable(true);
        schema1->set_index(0);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(1);
      }
    }

    // aggression
    pb_coprocessor.add_group_by_columns(0);
    pb_coprocessor.add_group_by_columns(1);

    coprocessor->Close();
    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }
}

TEST_F(CoprocessorTest, ExecuteNoAggregationValueOneEmpty) {
  butil::Status ok;

  std::string my_min_key(min_key.c_str(), 8);
  std::string my_max_key(max_key.c_str(), 8);

  std::string my_min_key_s = StrToHex(my_min_key, " ");
  std::cout << "my_min_key_s : " << my_min_key_s << '\n';

  std::string my_max_key_s = StrToHex(my_max_key, " ");
  std::cout << "my_max_key_s : " << my_max_key_s << '\n';

  IteratorOptions options;
  options.upper_bound = Helper::PrefixNext(my_max_key);
  auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

  bool key_only = false;
  size_t max_fetch_cnt = 2;
  int64_t max_bytes_rpc = 1000000000000000;
  std::vector<pb::common::KeyValue> kvs;

  iter->Seek(my_min_key);

  size_t cnt = 0;

  while (true) {
    bool has_more = true;
    ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
    EXPECT_EQ(ok.error_code(), pb::error::OK);
    cnt += kvs.size();
    if (kvs.empty()) {
      break;
    }
    kvs.clear();
  }
  std::cout << "key_values empty value cnt : " << cnt << '\n';
}

// without Aggregation only selection bad
TEST_F(CoprocessorTest, OpenBadSelection) {
  butil::Status ok;

  // ok no aggregation
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(2);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(3);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(5);
    }

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(true);
        schema1->set_is_nullable(true);
        schema1->set_index(0);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(1);
      }

      auto *schema3 = result_schema->add_schema();
      {
        schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
        schema3->set_is_key(false);
        schema3->set_is_nullable(true);
        schema3->set_index(2);
      }

      auto *schema4 = result_schema->add_schema();
      {
        schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema4->set_is_key(false);
        schema4->set_is_nullable(true);
        schema4->set_index(3);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(4);
      }

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(5);
      }
    }
    coprocessor->Close();

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }
}

TEST_F(CoprocessorTest, ExecuteBadSelection) {
  butil::Status ok;

  std::string my_min_key(min_key.c_str(), 8);
  std::string my_max_key(max_key.c_str(), 8);

  std::string my_min_key_s = StrToHex(my_min_key, " ");
  std::cout << "my_min_key_s : " << my_min_key_s << '\n';

  std::string my_max_key_s = StrToHex(my_max_key, " ");
  std::cout << "my_max_key_s : " << my_max_key_s << '\n';

  IteratorOptions options;
  options.upper_bound = Helper::PrefixNext(my_max_key);
  auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

  bool key_only = false;
  size_t max_fetch_cnt = 2;
  int64_t max_bytes_rpc = 1000000000000000;
  std::vector<pb::common::KeyValue> kvs;

  iter->Seek(my_min_key);

  size_t cnt = 0;

  while (true) {
    bool has_more = true;
    ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
    EXPECT_EQ(ok.error_code(), pb::error::OK);
    break;
  }
  std::cout << "key_values selection cnt : " << cnt << '\n';
}

TEST_F(CoprocessorTest, KvDeleteRange) {
  const std::string &cf_name = kDefaultCf;
  auto writer = engine->Writer();

  // ok
  {
    pb::common::Range range;

    std::string my_min_key(min_key.c_str(), 8);
    std::string my_max_key(max_key.c_str(), 8);

    std::string my_min_key_s = StrToHex(my_min_key, " ");
    std::cout << "my_min_key_s : " << my_min_key_s << '\n';

    std::string my_max_key_s = StrToHex(my_max_key, " ");
    std::cout << "my_max_key_s : " << my_max_key_s << '\n';

    range.set_start_key(my_min_key);
    range.set_end_key(Helper::PrefixNext(my_max_key));

    butil::Status ok = writer->KvDeleteRange(cf_name, range);

    EXPECT_EQ(ok.error_code(), dingodb::pb::error::Errno::OK);

    std::string start_key = my_min_key;
    std::string end_key = Helper::PrefixNext(my_max_key);
    std::vector<dingodb::pb::common::KeyValue> kvs;

    auto reader = engine->Reader();

    ok = reader->KvScan(cf_name, start_key, end_key, kvs);
    EXPECT_EQ(ok.error_code(), dingodb::pb::error::Errno::OK);

    std::cout << "start_key : " << StrToHex(start_key, " ") << "\n"
              << "end_key : " << StrToHex(end_key, " ") << '\n';
    for (const auto &kv : kvs) {
      std::cout << kv.key() << ":" << kv.value() << '\n';
    }
  }
}

TEST_F(CoprocessorTest, PrepareForDisorder) {
  int schema_version = 1;
  std::shared_ptr<std::vector<std::shared_ptr<BaseSchema>>> schemas;
  long common_id = 1;  // NOLINT

  schemas = std::make_shared<std::vector<std::shared_ptr<BaseSchema>>>();

  schemas->reserve(6);

  std::shared_ptr<DingoSchema<std::optional<std::shared_ptr<std::string>>>> string_schema =
      std::make_shared<DingoSchema<std::optional<std::shared_ptr<std::string>>>>();
  string_schema->SetIsKey(true);
  string_schema->SetAllowNull(true);
  string_schema->SetIndex(0);
  schemas->emplace_back(std::move(string_schema));

  std::shared_ptr<DingoSchema<std::optional<double>>> double_schema =
      std::make_shared<DingoSchema<std::optional<double>>>();
  double_schema->SetIsKey(true);
  double_schema->SetAllowNull(true);
  double_schema->SetIndex(1);
  schemas->emplace_back(std::move(double_schema));

  std::shared_ptr<DingoSchema<std::optional<bool>>> bool_schema = std::make_shared<DingoSchema<std::optional<bool>>>();
  bool_schema->SetIsKey(false);
  bool_schema->SetAllowNull(true);
  bool_schema->SetIndex(5);
  schemas->emplace_back(std::move(bool_schema));

  std::shared_ptr<DingoSchema<std::optional<int64_t>>> long_schema =
      std::make_shared<DingoSchema<std::optional<int64_t>>>();
  long_schema->SetIsKey(false);
  long_schema->SetAllowNull(true);
  long_schema->SetIndex(2);
  schemas->emplace_back(std::move(long_schema));

  std::shared_ptr<DingoSchema<std::optional<int32_t>>> int_schema =
      std::make_shared<DingoSchema<std::optional<int32_t>>>();
  int_schema->SetIsKey(false);
  int_schema->SetAllowNull(true);
  int_schema->SetIndex(4);
  schemas->emplace_back(std::move(int_schema));

  std::shared_ptr<DingoSchema<std::optional<float>>> float_schema =
      std::make_shared<DingoSchema<std::optional<float>>>();
  float_schema->SetIsKey(false);
  float_schema->SetAllowNull(true);
  float_schema->SetIndex(3);
  schemas->emplace_back(std::move(float_schema));

  RecordEncoder record_encoder(schema_version, schemas, common_id);

  // std::string min_key;
  // std::string max_key;

  // 1
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);

    // std::any any_string = std::optional<std::shared_ptr<std::string>>(std::nullopt);
    std::shared_ptr sp = std::make_shared<std::string>("cccc");
    std::any any_string = std::optional<std::shared_ptr<std::string>>(sp);
    record.emplace_back(any_string);

    // std::any any_double = std::optional<double>(std::nullopt);
    std::any any_double = std::optional<double>(0.0);
    record.emplace_back(any_double);

    // std::any any_long = std::optional<int64_t>(std::nullopt);
    std::any any_long = std::optional<int64_t>(0);
    record.emplace_back(any_long);

    // std::any any_float = std::optional<float>(std::nullopt);
    std::any any_float = std::optional<float>(0.0f);
    record.emplace_back(any_float);

    // std::any any_int = std::optional<int32_t>(std::nullopt);
    std::any any_int = std::optional<int32_t>(0);
    record.emplace_back(any_int);

    // std::any any_bool = std::optional<bool>(std::nullopt);
    std::any any_bool = std::optional<bool>(false);
    record.emplace_back(any_bool);

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 2
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("fdf45nrthn"));
    record.emplace_back(std::move(any_string));

    std::any any_double = std::optional<double>(23.4545);
    record.emplace_back(std::move(any_double));

    std::any any_long = std::optional<int64_t>(100);
    record.emplace_back(std::move(any_long));

    std::any any_float = std::optional<float>(1.23);
    record.emplace_back(std::move(any_float));

    std::any any_int = std::optional<int32_t>(1);
    record.emplace_back(std::move(any_int));

    std::any any_bool = std::optional<bool>(false);
    record.emplace_back(std::move(any_bool));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 3
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("sssfdf45nrthn"));
    record.emplace_back(std::move(any_string));

    std::any any_double = std::optional<double>(3443.5656);
    record.emplace_back(std::move(any_double));

    std::any any_long = std::optional<int64_t>(200);
    record.emplace_back(std::move(any_long));

    std::any any_float = std::optional<float>(2.23);
    record.emplace_back(std::move(any_float));

    std::any any_int = std::optional<int32_t>(2);
    record.emplace_back(std::move(any_int));

    std::any any_bool = std::optional<bool>(true);
    record.emplace_back(std::move(any_bool));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 4
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("cccfdf45nrthn"));
    record.emplace_back(std::move(any_string));

    std::any any_double = std::optional<double>(3434343443.56565);
    record.emplace_back(std::move(any_double));

    std::any any_long = std::optional<int64_t>(232545);
    record.emplace_back(std::move(any_long));

    std::any any_float = std::optional<float>(3.23);
    record.emplace_back(std::move(any_float));

    std::any any_int = std::optional<int32_t>(std::nullopt);
    record.emplace_back(std::move(any_int));

    std::any any_bool = std::optional<bool>(3);
    record.emplace_back(std::move(any_bool));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 5
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("errerfdf45nrthn"));
    record.emplace_back(std::move(any_string));

    std::any any_double = std::optional<double>(std::nullopt);
    record.emplace_back(std::move(any_double));

    std::any any_long = std::optional<int64_t>(std::nullopt);
    record.emplace_back(std::move(any_long));

    std::any any_float = std::optional<float>(4.23);
    record.emplace_back(std::move(any_float));

    std::any any_int = std::optional<int32_t>(4);
    record.emplace_back(std::move(any_int));

    std::any any_bool = std::optional<bool>(true);
    record.emplace_back(std::move(any_bool));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 6
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::nullopt);
    record.emplace_back(std::move(any_string));

    std::any any_double = std::optional<double>(99888343434);
    record.emplace_back(std::move(any_double));

    std::any any_long = std::optional<int64_t>(123455666);
    record.emplace_back(std::move(any_long));

    std::any any_float = std::optional<float>(5.23);
    record.emplace_back(std::move(any_float));

    std::any any_int = std::optional<int32_t>(std::nullopt);
    record.emplace_back(std::move(any_int));

    std::any any_bool = std::optional<bool>(5);
    record.emplace_back(std::move(any_bool));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    size_t min_size = std::min(key.size(), min_key.size());
    if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0 || min_key.empty()) {
      min_key = key;
    }

    min_size = std::min(key.size(), max_key.size());
    if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0 || max_key.empty()) {
      max_key = key;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 7
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::make_shared<std::string>("dfaerj56j"));
    record.emplace_back(std::move(any_string));

    std::any any_double = std::optional<double>(0.123232323);
    record.emplace_back(std::move(any_double));

    std::any any_long = std::optional<int64_t>(11111111);
    record.emplace_back(std::move(any_long));

    std::any any_float = std::optional<float>(6.23);
    record.emplace_back(std::move(any_float));

    std::any any_int = std::optional<int32_t>(6);
    record.emplace_back(std::move(any_int));

    std::any any_bool = std::optional<bool>(false);
    record.emplace_back(std::move(any_bool));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }

  // 8
  {
    pb::common::KeyValue key_value;
    std::vector<std::any> record;
    record.reserve(6);

    std::any any_string = std::optional<std::shared_ptr<std::string>>(std::nullopt);
    record.emplace_back(std::move(any_string));

    std::any any_double = std::optional<double>(454.343434);
    record.emplace_back(std::move(any_double));

    std::any any_long = std::optional<int64_t>(1111111111111);
    record.emplace_back(std::move(any_long));

    std::any any_float = std::optional<float>(7.23);
    record.emplace_back(std::move(any_float));

    std::any any_int = std::optional<int32_t>(7);
    record.emplace_back(std::move(any_int));

    std::any any_bool = std::optional<bool>(true);
    record.emplace_back(std::move(any_bool));

    int ret = record_encoder.Encode(record, key_value);

    EXPECT_EQ(ret, 0);

    butil::Status ok = engine->Writer()->KvPut(kDefaultCf, key_value);
    EXPECT_EQ(ok.error_code(), pb::error::OK);

    std::string key = key_value.key();
    std::string value = key_value.value();

    if (min_key.empty()) {
      min_min_size = key.size();
      min_key = key;
    } else {
      size_t min_size = std::min(key.size(), min_min_size);
      if (memcmp(key.c_str(), min_key.c_str(), min_size) < 0) {
        min_key = key;
      }
      min_min_size = min_size;
    }

    if (max_key.empty()) {
      max_key = key;
      max_min_size = key.size();
    } else {
      size_t min_size = std::min(key.size(), max_key.size());
      if (memcmp(key.c_str(), min_key.c_str(), min_size) > 0) {
        max_key = key;
      }
      max_min_size = min_size;
    }

    std::string s = StrToHex(key, " ");
    std::cout << "s : " << s << '\n';
  }
}

// only has expr disorder ok
TEST_F(CoprocessorTest, OpenAndExecuteDisorderExpr) {
  butil::Status ok;

  // open ok no aggregation and group by key
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(5);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema2->set_is_key(false);
      schema2->set_is_nullable(true);
      schema2->set_index(4);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(3);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(2);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema5->set_is_key(true);
      schema5->set_is_nullable(true);
      schema5->set_index(1);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema6->set_is_key(true);
      schema6->set_is_nullable(true);
      schema6->set_index(0);
    }

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(0);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(1);
      }

      auto *schema4 = result_schema->add_schema();
      {
        schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema4->set_is_key(false);
        schema4->set_is_nullable(true);
        schema4->set_index(2);
      }

      auto *schema3 = result_schema->add_schema();
      {
        schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
        schema3->set_is_key(false);
        schema3->set_is_nullable(true);
        schema3->set_index(3);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(4);
      }

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(true);
        schema1->set_is_nullable(true);
        schema1->set_index(5);
      }
    }
    coprocessor->Close();

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }

  // exection
  {
    std::string my_min_key(min_key.c_str(), 8);
    std::string my_max_key(max_key.c_str(), 8);

    std::string my_min_key_s = StrToHex(my_min_key, " ");
    std::cout << "my_min_key_s : " << my_min_key_s << '\n';

    std::string my_max_key_s = StrToHex(my_max_key, " ");
    std::cout << "my_max_key_s : " << my_max_key_s << '\n';

    IteratorOptions options;
    options.upper_bound = Helper::PrefixNext(my_max_key);
    auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

    bool key_only = false;
    size_t max_fetch_cnt = 2;
    int64_t max_bytes_rpc = 1000000000000000;
    std::vector<pb::common::KeyValue> kvs;

    iter->Seek(my_min_key);

    size_t cnt = 0;

    while (true) {
      bool has_more = true;
      ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
      EXPECT_EQ(ok.error_code(), pb::error::OK);
      break;
    }
    std::cout << "key_values selection cnt : " << cnt << '\n';
  }
}

// group by key disorder ok
TEST_F(CoprocessorTest, OpenAndExecuteDisorderGroupByKey) {
  butil::Status ok;

  // open ok no aggregation and group by key
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema2->set_is_key(true);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(5);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(2);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema5->set_is_key(false);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema6->set_is_key(false);
      schema6->set_is_nullable(true);
      schema6->set_index(3);
    }

    {
      // group by key 0 = string 1 = double
      pb_coprocessor.add_group_by_columns(0);
      pb_coprocessor.add_group_by_columns(1);
    }

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(0);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(1);
      }
    }
    coprocessor->Close();

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }

  // execution
  {
    std::string my_min_key(min_key.c_str(), 8);
    std::string my_max_key(max_key.c_str(), 8);

    std::string my_min_key_s = StrToHex(my_min_key, " ");
    std::cout << "my_min_key_s : " << my_min_key_s << '\n';

    std::string my_max_key_s = StrToHex(my_max_key, " ");
    std::cout << "my_max_key_s : " << my_max_key_s << '\n';

    IteratorOptions options;
    options.upper_bound = Helper::PrefixNext(my_max_key);
    auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

    bool key_only = false;
    size_t max_fetch_cnt = 2;
    int64_t max_bytes_rpc = 1000000000000000;
    std::vector<pb::common::KeyValue> kvs;

    iter->Seek(my_min_key);

    size_t cnt = 0;

    while (true) {
      bool has_more = true;
      ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
      EXPECT_EQ(ok.error_code(), pb::error::OK);
      break;
    }
    std::cout << "key_values selection cnt : " << cnt << '\n';
  }
}

// only has aggregation. no group by key ok.
TEST_F(CoprocessorTest, OpenAndExecuteDisorderAggregation) {
  butil::Status ok;

  // open ok no aggregation and group by key
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema2->set_is_key(true);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(5);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(2);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema5->set_is_key(false);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema6->set_is_key(false);
      schema6->set_is_nullable(true);
      schema6->set_index(3);
    }

    // aggregation
    {
      auto *aggregation_operator1 = pb_coprocessor.add_aggregation_operators();
      {
        // string
        aggregation_operator1->set_oper(::dingodb::pb::store::AggregationType::COUNT);
        aggregation_operator1->set_index_of_column(0);
      }

      auto *aggregation_operator2 = pb_coprocessor.add_aggregation_operators();
      {
        // double
        aggregation_operator2->set_oper(::dingodb::pb::store::AggregationType::SUM);
        aggregation_operator2->set_index_of_column(1);
      }

      auto *aggregation_operator3 = pb_coprocessor.add_aggregation_operators();
      {
        // long
        aggregation_operator3->set_oper(::dingodb::pb::store::AggregationType::COUNTWITHNULL);
        aggregation_operator3->set_index_of_column(2);
      }

      auto *aggregation_operator4 = pb_coprocessor.add_aggregation_operators();
      {
        // float
        aggregation_operator4->set_oper(::dingodb::pb::store::AggregationType::MAX);
        aggregation_operator4->set_index_of_column(3);
      }

      auto *aggregation_operator5 = pb_coprocessor.add_aggregation_operators();
      {
        // int32
        aggregation_operator5->set_oper(::dingodb::pb::store::AggregationType::SUM0);
        aggregation_operator5->set_index_of_column(4);
      }

      auto *aggregation_operator6 = pb_coprocessor.add_aggregation_operators();
      {
        // bool
        aggregation_operator6->set_oper(::dingodb::pb::store::AggregationType::MIN);
        aggregation_operator6->set_index_of_column(5);
      }
    }

    // result
    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(0);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(1);
      }

      auto *schema4 = result_schema->add_schema();
      {
        schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema4->set_is_key(false);
        schema4->set_is_nullable(true);
        schema4->set_index(2);
      }

      auto *schema3 = result_schema->add_schema();
      {
        schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
        schema3->set_is_key(false);
        schema3->set_is_nullable(true);
        schema3->set_index(3);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(4);
      }

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(true);
        schema1->set_is_nullable(true);
        schema1->set_index(5);
      }
    }
    coprocessor->Close();

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }

  // execution
  {
    std::string my_min_key(min_key.c_str(), 8);
    std::string my_max_key(max_key.c_str(), 8);

    std::string my_min_key_s = StrToHex(my_min_key, " ");
    std::cout << "my_min_key_s : " << my_min_key_s << '\n';

    std::string my_max_key_s = StrToHex(my_max_key, " ");
    std::cout << "my_max_key_s : " << my_max_key_s << '\n';

    IteratorOptions options;
    options.upper_bound = Helper::PrefixNext(my_max_key);
    auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

    bool key_only = false;
    size_t max_fetch_cnt = 2;
    int64_t max_bytes_rpc = 1000000000000000;
    std::vector<pb::common::KeyValue> kvs;

    iter->Seek(my_min_key);

    size_t cnt = 0;

    while (true) {
      bool has_more = true;
      ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
      EXPECT_EQ(ok.error_code(), pb::error::OK);
      break;
    }
    std::cout << "key_values selection cnt : " << cnt << '\n';
  }
}

// only has aggregation and  group by key ok.
TEST_F(CoprocessorTest, OpenAndExecuteDisorderAggregationAndGroupByKey) {
  butil::Status ok;

  // open ok no aggregation and group by key
  {
    pb::store::Coprocessor pb_coprocessor;

    pb_coprocessor.set_schema_version(1);

    auto *original_schema = pb_coprocessor.mutable_original_schema();
    original_schema->set_common_id(1);

    auto *schema1 = original_schema->add_schema();
    {
      schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
      schema1->set_is_key(true);
      schema1->set_is_nullable(true);
      schema1->set_index(0);
    }

    auto *schema2 = original_schema->add_schema();
    {
      schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
      schema2->set_is_key(true);
      schema2->set_is_nullable(true);
      schema2->set_index(1);
    }

    auto *schema3 = original_schema->add_schema();
    {
      schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
      schema3->set_is_key(false);
      schema3->set_is_nullable(true);
      schema3->set_index(5);
    }

    auto *schema4 = original_schema->add_schema();
    {
      schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
      schema4->set_is_key(false);
      schema4->set_is_nullable(true);
      schema4->set_index(2);
    }

    auto *schema5 = original_schema->add_schema();
    {
      schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
      schema5->set_is_key(false);
      schema5->set_is_nullable(true);
      schema5->set_index(4);
    }

    auto *schema6 = original_schema->add_schema();
    {
      schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
      schema6->set_is_key(false);
      schema6->set_is_nullable(true);
      schema6->set_index(3);
    }

    // group by key 0 = string 1 = double
    {
      pb_coprocessor.add_group_by_columns(0);
      pb_coprocessor.add_group_by_columns(1);
    }

    // aggregation
    {
      auto *aggregation_operator1 = pb_coprocessor.add_aggregation_operators();
      {
        aggregation_operator1->set_oper(::dingodb::pb::store::AggregationType::COUNT);
        aggregation_operator1->set_index_of_column(0);
      }

      auto *aggregation_operator2 = pb_coprocessor.add_aggregation_operators();
      {
        aggregation_operator2->set_oper(::dingodb::pb::store::AggregationType::SUM);
        aggregation_operator2->set_index_of_column(1);
      }

      auto *aggregation_operator3 = pb_coprocessor.add_aggregation_operators();
      {
        aggregation_operator3->set_oper(::dingodb::pb::store::AggregationType::COUNTWITHNULL);
        aggregation_operator3->set_index_of_column(2);
      }

      auto *aggregation_operator4 = pb_coprocessor.add_aggregation_operators();
      {
        aggregation_operator4->set_oper(::dingodb::pb::store::AggregationType::MAX);
        aggregation_operator4->set_index_of_column(3);
      }

      auto *aggregation_operator5 = pb_coprocessor.add_aggregation_operators();
      {
        aggregation_operator5->set_oper(::dingodb::pb::store::AggregationType::SUM0);
        aggregation_operator5->set_index_of_column(4);
      }

      auto *aggregation_operator6 = pb_coprocessor.add_aggregation_operators();
      {
        aggregation_operator6->set_oper(::dingodb::pb::store::AggregationType::MIN);
        aggregation_operator6->set_index_of_column(5);
      }
    }

    {
      auto *result_schema = pb_coprocessor.mutable_result_schema();
      result_schema->set_common_id(1);

      auto *schema8 = result_schema->add_schema();
      {
        schema8->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_STRING);
        schema8->set_is_key(true);
        schema8->set_is_nullable(true);
        schema8->set_index(0);
      }

      auto *schema7 = result_schema->add_schema();
      {
        schema7->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema7->set_is_key(true);
        schema7->set_is_nullable(true);
        schema7->set_index(1);
      }

      ////////////////////////////////// aggregation

      auto *schema6 = result_schema->add_schema();
      {
        schema6->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema6->set_is_key(true);
        schema6->set_is_nullable(true);
        schema6->set_index(2);
      }

      auto *schema5 = result_schema->add_schema();
      {
        schema5->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_DOUBLE);
        schema5->set_is_key(true);
        schema5->set_is_nullable(true);
        schema5->set_index(3);
      }

      auto *schema4 = result_schema->add_schema();
      {
        schema4->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_LONG);
        schema4->set_is_key(false);
        schema4->set_is_nullable(true);
        schema4->set_index(4);
      }

      auto *schema3 = result_schema->add_schema();
      {
        schema3->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_FLOAT);
        schema3->set_is_key(false);
        schema3->set_is_nullable(true);
        schema3->set_index(5);
      }

      auto *schema2 = result_schema->add_schema();
      {
        schema2->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_INTEGER);
        schema2->set_is_key(false);
        schema2->set_is_nullable(true);
        schema2->set_index(6);
      }

      auto *schema1 = result_schema->add_schema();
      {
        schema1->set_type(::dingodb::pb::common::Schema_Type::Schema_Type_BOOL);
        schema1->set_is_key(true);
        schema1->set_is_nullable(true);
        schema1->set_index(7);
      }
    }
    coprocessor->Close();

    ok = coprocessor->Open(CoprocessorPbWrapper{pb_coprocessor});
    EXPECT_EQ(ok.error_code(), pb::error::OK);
  }

  // exection
  {
    std::string my_min_key(min_key.c_str(), 8);
    std::string my_max_key(max_key.c_str(), 8);

    std::string my_min_key_s = StrToHex(my_min_key, " ");
    std::cout << "my_min_key_s : " << my_min_key_s << '\n';

    std::string my_max_key_s = StrToHex(my_max_key, " ");
    std::cout << "my_max_key_s : " << my_max_key_s << '\n';

    IteratorOptions options;
    options.upper_bound = Helper::PrefixNext(my_max_key);
    auto iter = engine->Reader()->NewIterator(kDefaultCf, options);

    bool key_only = false;
    size_t max_fetch_cnt = 2;
    int64_t max_bytes_rpc = 1000000000000000;
    std::vector<pb::common::KeyValue> kvs;

    iter->Seek(my_min_key);

    size_t cnt = 0;

    while (true) {
      bool has_more = true;
      ok = coprocessor->Execute(iter, key_only, max_fetch_cnt, max_bytes_rpc, &kvs, has_more);
      EXPECT_EQ(ok.error_code(), pb::error::OK);
      break;
    }
    std::cout << "key_values selection cnt : " << cnt << '\n';
  }
}

TEST_F(CoprocessorTest, KvDeleteRangeForDisorder) {
  const std::string &cf_name = kDefaultCf;
  auto writer = engine->Writer();

  // ok
  {
    pb::common::Range range;

    std::string my_min_key(min_key.c_str(), 8);
    std::string my_max_key(max_key.c_str(), 8);

    std::string my_min_key_s = StrToHex(my_min_key, " ");
    std::cout << "my_min_key_s : " << my_min_key_s << '\n';

    std::string my_max_key_s = StrToHex(my_max_key, " ");
    std::cout << "my_max_key_s : " << my_max_key_s << '\n';

    range.set_start_key(my_min_key);
    range.set_end_key(Helper::PrefixNext(my_max_key));

    butil::Status ok = writer->KvDeleteRange(kDefaultCf, range);

    EXPECT_EQ(ok.error_code(), dingodb::pb::error::Errno::OK);

    std::string start_key = my_min_key;
    std::string end_key = Helper::PrefixNext(my_max_key);
    std::vector<dingodb::pb::common::KeyValue> kvs;

    auto reader = engine->Reader();

    ok = reader->KvScan(cf_name, start_key, end_key, kvs);
    EXPECT_EQ(ok.error_code(), dingodb::pb::error::Errno::OK);

    std::cout << "start_key : " << StrToHex(start_key, " ") << "\n"
              << "end_key : " << StrToHex(end_key, " ") << '\n';
    for (const auto &kv : kvs) {
      std::cout << kv.key() << ":" << kv.value() << '\n';
    }
  }
}

}  // namespace dingodb
