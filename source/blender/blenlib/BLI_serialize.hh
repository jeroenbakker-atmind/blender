/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

/** \file
 * \ingroup bli
 *
 * An abstraction layer between serialization formats. Currently only
 * support JSON.
 */

#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include <ostream>

#pragma once

namespace blender::io::serialize {

enum class eValueType {
  String,
  Int,
  Array,
  Null,
  Boolean,
  Float,
  Object,
};

class Value;
class StringValue;
class ObjectValue;
template<typename T, eValueType V> class PrimitiveValue;
using IntValue = PrimitiveValue<uint64_t, eValueType::Int>;
using FloatValue = PrimitiveValue<double, eValueType::Float>;
using BooleanValue = PrimitiveValue<bool, eValueType::Boolean>;

template<typename Container, typename ContainerItem, eValueType V> class ContainerValue;
using ArrayValue =
    ContainerValue<Vector<std::shared_ptr<Value>>, std::shared_ptr<Value>, eValueType::Array>;

class Value {
 private:
  eValueType _type;

 protected:
  Value() = delete;
  Value(eValueType type) : _type(type)
  {
  }

 public:
  virtual ~Value() = default;
  const eValueType type() const
  {
    return _type;
  }

  const StringValue *as_string_value() const;
  const IntValue *as_int_value() const;
  const FloatValue *as_float_value() const;
  const BooleanValue *as_boolean_value() const;
  const ArrayValue *as_array_value() const;
  const ObjectValue *as_object_value() const;
};

template<typename T, eValueType V> class PrimitiveValue : public Value {
 private:
  T _inner_value;

 public:
  PrimitiveValue(const T value) : Value(V), _inner_value(value)
  {
  }

  const T value() const
  {
    return _inner_value;
  }
};

class NullValue : public Value {
 public:
  NullValue() : Value(eValueType::Null)
  {
  }
};

class StringValue : public Value {
 private:
  std::string _string;

 public:
  StringValue(const StringRef string) : Value(eValueType::String), _string(string)
  {
  }

  const std::string &string_value() const
  {
    return _string;
  }
};

template<typename Container, typename ContainerItem, eValueType V>
class ContainerValue : public Value {
 public:
  using Items = Container;
  using Item = ContainerItem;

 private:
  Container _inner_value;

 public:
  ContainerValue() : Value(V)
  {
  }

  const Container &elements() const
  {
    return _inner_value;
  }

  Container &elements()
  {
    return _inner_value;
  }
};

/**
 * Object is a key value container where the key must by a std::string.
 * Internally it is stored in a blender::Vector to ensure the order of keys.
 */
class ObjectValue : public ContainerValue<Vector<std::pair<std::string, std::shared_ptr<Value>>>,
                                          std::pair<std::string, std::shared_ptr<Value>>,
                                          eValueType::Object> {
  using LookupValue = std::shared_ptr<Value>;
  using Lookup = Map<std::string, LookupValue>;

 public:
  /** Return a lookup map to quickly lookup by key. */
  const Lookup create_lookup() const
  {
    Lookup result;
    for (const Item &item : elements()) {
      result.add_as(item.first, item.second);
    }
    return result;
  }
};

class Formatter {
 public:
  virtual void serialize(std::ostream &os, Value &value) = 0;
  virtual Value *deserialize(std::istream &is) = 0;
};

class JsonFormatter : public Formatter {
 public:
  uint8_t indentation_len = 0;

 public:
  void serialize(std::ostream &os, Value &value) override;
  Value *deserialize(std::istream &is) override;
};

}  // namespace blender::io::serialize
