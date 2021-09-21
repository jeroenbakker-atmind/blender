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
  StringValue test_value("Hello JSON");
  json.serialize(out, test_value);
  EXPECT_EQ(out.str(), "\"Hello JSON\"");
}

TEST(serialize, int_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  IntValue test_value(42);
  json.serialize(out, test_value);
  EXPECT_EQ(out.str(), "42");
}

TEST(serialize, float_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  FloatValue test_value(42.31);
  json.serialize(out, test_value);
  EXPECT_EQ(out.str(), "42.31");
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
  BooleanValue value(false);
  json.serialize(out, value);
  EXPECT_EQ(out.str(), "false");
}

TEST(serialize, true_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  BooleanValue value(true);
  json.serialize(out, value);
  EXPECT_EQ(out.str(), "true");
}

TEST(serialize, array_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value value_array(ValueType::Array);
  Vector<Value *> &array = value_array.array_items();
  array.append_as(new IntValue(42));
  array.append_as(new StringValue("Hello JSON"));
  array.append_as(new Value(ValueType::Null));
  array.append_as(new BooleanValue(false));
  array.append_as(new BooleanValue(true));

  json.serialize(out, value_array);
  EXPECT_EQ(out.str(), "[42,\"Hello JSON\",null,false,true]");
}

TEST(serialize, object_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value value_object(ValueType::Object);
  Map<std::string, Value *> &attributes = value_object.attributes();
  attributes.add_as(std::string("best_number"), new IntValue(42));

  json.serialize(out, value_object);
  EXPECT_EQ(out.str(), "{\"best_number\":42}");
}

}  // namespace blender::io::serialize::json::testing
