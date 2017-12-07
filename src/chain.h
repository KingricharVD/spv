// Copyright (c) 2017 Evan Klitzke <evan@eklitzke.org>
//
// This file is part of SPV.
//
// SPV is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// SPV is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// SPV. If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <memory>

#include "./fields.h"

namespace spv {
class Client;

class Chain {
  friend Client;

 public:
  Chain(const Chain &other) = delete;
  Chain(const BlockHeader &hdr, size_t height) : hdr_(hdr) {
    hdr_.height = height;
  }

  BlockHeader tip() const;

  void add_child(const BlockHeader &hdr);

 private:
  BlockHeader hdr_;
  std::vector<std::unique_ptr<Chain>> children_;

  void tip_helper(BlockHeader &hdr) const;

 protected:
  Chain() : hdr_(BlockHeader::genesis()) {}
};
}  // namespace spv
