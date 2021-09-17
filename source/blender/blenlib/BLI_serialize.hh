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
};

class Value {
 private:
  ValueType _type;
  std::string _string_value;
  int64_t _int_value;

 public:
  Value(std::string value) : _type(ValueType::String), _string_value(value)
  {
  }

  Value(int64_t value) : _type(ValueType::Int), _int_value(value)
  {
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
};

class JsonFormatter {
 public:
  void serialize(std::ostream &os, Value &value);
};

}  // namespace blender::io::serialize
