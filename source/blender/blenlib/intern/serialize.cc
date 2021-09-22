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

static void convert_to_json(nlohmann::ordered_json &j, const Value &value)
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
      /* Force to use array in case we don't have any items. */
      j = "[]"_json;
      for (const ArrayValue::Item &item_value : items) {
        nlohmann::ordered_json json_item;
        convert_to_json(json_item, *item_value);
        j.push_back(json_item);
      }
      break;
    }

    case eValueType::Object: {
      const ObjectValue::Items &attributes = value.as_object_value()->elements();
      j = "{}"_json;
      for (const ObjectValue::Item &attribute : attributes) {
        nlohmann::ordered_json json_item;
        convert_to_json(json_item, *attribute.second);
        j[attribute.first] = json_item;
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

static Value *convert_from_json(const nlohmann::ordered_json &j)
{
  switch (j.type()) {
    case nlohmann::json::value_t::array: {
      ArrayValue *array = new ArrayValue();
      ArrayValue::Items &elements = array->elements();
      for (auto element : j.items()) {
        nlohmann::ordered_json element_json = element.value();
        Value *value = convert_from_json(element_json);
        elements.append_as(value);
      }
      return array;
    }

    case nlohmann::json::value_t::object: {
      ObjectValue *object = new ObjectValue();
      ObjectValue::Items &elements = object->elements();
      for (auto element : j.items()) {
        std::string key = element.key();
        nlohmann::ordered_json element_json = element.value();
        Value *value = convert_from_json(element_json);
        elements.append_as(std::pair(key, value));
      }
      return object;
    }

    case nlohmann::json::value_t::string: {
      std::string value = j;
      return new StringValue(value);
    }

    case nlohmann::json::value_t::null: {
      return new NullValue();
    }

    case nlohmann::json::value_t::boolean: {
      return new BooleanValue(j);
    }
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned: {
      return new IntValue(j);
    }

    case nlohmann::json::value_t::number_float: {
      return new FloatValue(j);
    }

    case nlohmann::json::value_t::binary:
    case nlohmann::json::value_t::discarded:
      /* Binary and discarded are not actual json elements. */
      BLI_assert_unreachable();
      return nullptr;
  }

  BLI_assert_unreachable();
  return nullptr;
}

void JsonFormatter::serialize(std::ostream &os, Value &value)
{
  nlohmann::ordered_json j;
  convert_to_json(j, value);
  if (indentation_len) {
    os << j.dump(indentation_len);
  }
  else {
    os << j.dump();
  }
}

Value *JsonFormatter::deserialize(std::istream &is)
{
  nlohmann::ordered_json j;
  is >> j;
  Value *value = convert_from_json(j);
  return value;
}

}  // namespace blender::io::serialize
