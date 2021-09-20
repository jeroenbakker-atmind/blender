/* Apache License, Version 2.0 */

#include "testing/testing.h"

#include "BLI_serialize.hh"

/* -------------------------------------------------------------------- */
/* tests */

namespace blender::io::serialize::json::testing {

TEST(serialize, string_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value test_value(ValueType::String, std::string("Hello JSON"));
  json.serialize(out, test_value);
  EXPECT_EQ(out.str(), "\"Hello JSON\"");
}

TEST(serialize, int_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value test_value(ValueType::Int, 42);
  json.serialize(out, test_value);
  EXPECT_EQ(out.str(), "42");
}

TEST(serialize, null_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value test_value(ValueType::Null);
  json.serialize(out, test_value);
  EXPECT_EQ(out.str(), "null");
}

TEST(serialize, false_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value value(ValueType::Boolean, false);
  json.serialize(out, value);
  EXPECT_EQ(out.str(), "false");
}

TEST(serialize, true_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value value(ValueType::Boolean, true);
  json.serialize(out, value);
  EXPECT_EQ(out.str(), "true");
}

TEST(serialize, array_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Vector<Value *> array;
  array.append_as(new Value(ValueType::Int, 42));
  array.append_as(new Value(ValueType::String, std::string("Hello JSON")));
  Value value_array(ValueType::Array, array);

  json.serialize(out, value_array);
  EXPECT_EQ(out.str(), "[42,\"Hello JSON\"]");
}

}  // namespace blender::io::serialize::json::testing
