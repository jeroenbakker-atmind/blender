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

namespace blender::io::serialize {

enum class ValueType {
  String,
  Int,
  Array,
  Null,
  Boolean,
};

class Value {
 private:
  ValueType _type;
  union {
    std::string _string_value = "";
    int64_t _int_value;
    Vector<Value *> _array_items;
    bool _boolean;
  };

 public:
  explicit Value(ValueType type, StringRef value) : _type(type)
  {
    BLI_assert_msg(type == ValueType::String,
                   "Only string types can be used by `Value(type, string) constructor");

    switch (_type) {
      case ValueType::String: {
        _string_value = value;
        break;
      }
      case ValueType::Boolean:
      case ValueType::Null:
      case ValueType::Array:
      case ValueType::Int:
        /* Unsupported by this constructor. */
        break;
    }
  }

  explicit Value(ValueType type, int64_t value) : _type(type)
  {
    BLI_assert_msg(ELEM(type, ValueType::Int, ValueType::Boolean),
                   "Only integer and boolean types can be used by `Value(type, int)` constructor");

    switch (_type) {
      case ValueType::Int:
        _int_value = value;
        break;

      case ValueType::Boolean:
        _boolean = value != 0;
        break;

      case ValueType::Null:
      case ValueType::Array:
      case ValueType::String:
        /* Unsupported by this constructor. */
        break;
    }
  }

  explicit Value(ValueType type) : _type(type)
  {
    BLI_assert_msg(type == ValueType::Null,
                   "Only null types can be used by `Value(type)` constructor");
  }

  explicit Value(ValueType type, Vector<Value *> &items) : _type(type), _array_items(items)
  {
    BLI_assert_msg(type == ValueType::Array,
                   "Only array types can be used by `Value(type, vector)` constructor");
    switch (_type) {
      case ValueType::Array:
        _array_items = items;
        break;

      case ValueType::Boolean:
      case ValueType::Int:
      case ValueType::Null:
      case ValueType::String:
        /* Unsupported by this constructor. */
        break;
    }
  }

  ~Value()
  {
    switch (_type) {
      case ValueType::Array: {
        while (!_array_items.is_empty()) {
          Value *value = _array_items.pop_last();
          delete value;
        }
        _array_items.clear_and_make_inline();
        break;
      }
      case ValueType::String:
      case ValueType::Int:
      case ValueType::Null:
      case ValueType::Boolean:
        /* Nothing to delete for simple types. */
        break;
    }
  }

  const ValueType type() const
  {
    return _type;
  }

  const std::string &string_value() const
  {
    BLI_assert(_type == ValueType::String);
    return _string_value;
  }

  const int64_t &int_value() const
  {
    BLI_assert(_type == ValueType::Int);
    return _int_value;
  }

  const bool &boolean_value() const
  {
    BLI_assert(_type == ValueType::Boolean);
    return _boolean;
  }

  const Vector<Value *> &array_items() const
  {
    BLI_assert(_type == ValueType::Array);
    return _array_items;
  }
};

class JsonFormatter {
 public:
  uint8_t indentation_len = 0;

 public:
  void serialize(std::ostream &os, Value &value);
};

}  // namespace blender::io::serialize
