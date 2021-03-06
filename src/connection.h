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
#include <utility>

#include "./addr.h"
#include "./buffer.h"
#include "./config.h"
#include "./message.h"
#include "./peer.h"
#include "./util.h"

namespace uvw {
class TcpHandle;
class TimerHandle;
class Loop;
class Addr;
}  // namespace uvw

namespace spv {

class Client;
class Connection {
  friend Client;

 public:
  Connection() = delete;
  Connection(Client* client_, const Addr& addr);
  Connection(const Connection& other) = delete;
  ~Connection() { shutdown(); }

  const Peer& peer() const { return peer_; }

  // establish the connection
  void connect();

  // read data
  void read(const char* data, size_t sz);

  void _version();

  inline bool connected() const { return have_version_ && have_verack_; }

 private:
  std::shared_ptr<uvw::Loop> loop_;
  Client* client_;
  Buffer buf_;
  Peer peer_;

  bool have_version_;
  bool have_verack_;

 protected:
  std::shared_ptr<uvw::TcpHandle> tcp_;

  // close this connection (e.g. because we have a bad peer)
  void shutdown();

  // request headers
  void get_headers(const std::vector<hash_t>& locator_hashes,
                   const hash_t& hash_stop = empty_hash);
  void get_headers(const BlockHeader& start_hdr);
  void get_data(const Inv& inv);
  void send_version();

 private:
  // heartbeat information
  uint64_t ping_nonce_;
  std::shared_ptr<uvw::TimerHandle> ping_;
  std::shared_ptr<uvw::TimerHandle> pong_;
  std::shared_ptr<uvw::TimerHandle> verack_;
  std::shared_ptr<uvw::TimerHandle> getaddr_;

  // returns true if a message was actually read
  bool read_message();

  // send a message to our peer
  void send_msg(const Message& msg);

  void handle_addr(AddrMsg* addrs);
  void handle_getaddr(GetAddr* getaddr);
  void handle_getblocks(GetBlocks* getblocks);
  void handle_getheaders(GetHeaders* getheaders);
  void handle_headers(HeadersMsg* headers);
  void handle_inv(InvMsg* inv);
  void handle_mempool(Mempool* pool);
  void handle_ping(Ping* ping);
  void handle_pong(Pong* pong);
  void handle_reject(Reject* rej);
  void handle_sendheaders(SendHeaders* send);
  void handle_unknown(const std::string& msg);
  void handle_verack(VerAck* ack);
  void handle_version(Version* ver);

  void get_new_addrs();
};
}  // namespace spv

std::ostream& operator<<(std::ostream& o, const spv::Connection& conn);
