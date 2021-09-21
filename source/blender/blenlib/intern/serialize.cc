#include "BLI_serialize.hh"

#include "json.hpp"

namespace blender::io::serialize {

const StringValue *Value::as_string_value() const
{
  if (_type != eValueType::String) {
    return nullptr;
  }
  return static_cast<const StringValue *>(this);
}

const IntValue *Value::as_int_value() const
{
  if (_type != eValueType::Int) {
    return nullptr;
  }
  return static_cast<const IntValue *>(this);
}

const FloatValue *Value::as_float_value() const
{
  if (_type != eValueType::Float) {
    return nullptr;
  }
  return static_cast<const FloatValue *>(this);
}

const BooleanValue *Value::as_boolean_value() const
{
  if (_type != eValueType::Boolean) {
    return nullptr;
  }
  return static_cast<const BooleanValue *>(this);
}

const ArrayValue *Value::as_array_value() const
{
  if (_type != eValueType::Array) {
    return nullptr;
  }
  return static_cast<const ArrayValue *>(this);
}

const ObjectValue *Value::as_object_value() const
{
  if (_type != eValueType::Object) {
    return nullptr;
  }
  return static_cast<const ObjectValue *>(this);
}

static void convert_to_json(nlohmann::json &j, const Value &value)
{
  switch (value.type()) {
    case eValueType::String: {
      j = value.as_string_value()->string_value();
      break;
    }

    case eValueType::Int: {
      j = value.as_int_value()->value();
      break;
    }

    case eValueType::Array: {
      const ArrayValue::Items &items = value.as_array_value()->elements();
      for (const ArrayValue::Item &item_value : items) {
        nlohmann::json json_item;
        convert_to_json(json_item, *item_value);
        j.push_back(json_item);
      }
      break;
    }

    case eValueType::Object: {
      const ObjectValue::Items &attributes = value.as_object_value()->elements();
      for (const ObjectValue::Items::Item &attribute : attributes.items()) {
        nlohmann::json json_item;
        convert_to_json(json_item, *attribute.value);
        j[attribute.key] = json_item;
      }
      break;
    }

    case eValueType::Null: {
      j = nullptr;
      break;
    }

    case eValueType::Boolean: {
      j = value.as_boolean_value()->value();
      break;
    }

    case eValueType::Float: {
      j = value.as_float_value()->value();
    }
  }
}

void JsonFormatter::serialize(std::ostream &os, Value &value)
{
  nlohmann::json j;
  convert_to_json(j, value);
  if (indentation_len) {
    os << j.dump(indentation_len);
  }
  else {
    os << j.dump();
  }
}

}  // namespace blender::io::serialize
