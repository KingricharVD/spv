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

#include "./client.h"

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <iostream>

#include "./logging.h"
#include "./util.h"

void log_handle(uvw::BaseHandle &handle) {
  std::cerr << "leaked a handle: " << &handle << "\n";
}

void walk_handles() {
  auto loop = uvw::Loop::getDefault();
  loop->walk(log_handle);
}

namespace spv {
DEFINE_LOGGER

static const std::chrono::seconds NO_REPEAT{0};
static const std::chrono::seconds CONNECT_TIMEOUT{1};

// copied from chainparams.cpp
static const std::vector<std::string> testSeeds = {
    "testnet-seed.bitcoin.jonasschnelli.ch", "seed.tbtc.petertodd.org",
    "testnet-seed.bluematt.me",
};

enum { PROTOCOL_VERSION = 70001 };

// testnet port
enum { DEFAULT_PORT = 18332 };

void Client::run() {
  for (const auto &seed : testSeeds) {
    lookup_seed(seed);
  }

  auto loop = uvw::Loop::getDefault();
  auto timer = loop->resource<uvw::TimerHandle>();
  timer->on<uvw::TimerEvent>([this](const auto &, auto &timer) {
    log->warn("initiating shutdown");
    timer.close();
    shutdown();
    walk_handles();
  });
  timer->start(std::chrono::seconds(10), NO_REPEAT);
}

// for debugging only
void Client::shutdown() {
  log->warn("shutting down");

  for (auto &conn : connections_) {
    log->info("conn has use_count {}", conn.use_count());
    conn->close();
  }
  connections_.clear();

  uvw::Loop::getDefault()->stop();
}

void Client::send_version(const NetAddr &addr) {
  Encoder enc("version");
  enc.push_int<int32_t>(PROTOCOL_VERSION);    // version
  enc.push_int<uint64_t>(0);                  // services
  enc.push_time<uint64_t>();                  // timestamp
  enc.push_addr(&addr);                       // addr_recv
  enc.push_addr(nullptr);                     // addr_from
  enc.push_int<uint64_t>(connection_nonce_);  // nonce
  enc.push_string(USERAGENT);                 // user-agent
  enc.push_int<uint32_t>(1);                  // start height
  enc.push_bool(false);                       // relay

  log->info("version message is: {}", string_to_hex(enc.serialize()));
}

void Client::lookup_seed(const std::string &seed) {
  auto loop = uvw::Loop::getDefault();
  auto request = loop->resource<uvw::GetAddrInfoReq>();
  request->on<uvw::ErrorEvent>(
      [](const auto &, auto &) { log->error("dns resolution failed"); });
  request->on<uvw::AddrInfoEvent>([this, seed](const auto &event, auto &) {
    char buf[INET6_ADDRSTRLEN];
    memset(buf, 0, sizeof buf);

    sockaddr_in *sa4;
    sockaddr_in6 *sa6;
    uvw::Addr addr;
    addr.port = DEFAULT_PORT;

    for (const addrinfo *p = event.data.get(); p != nullptr; p = p->ai_next) {
      switch (p->ai_addr->sa_family) {
        case AF_INET:
          sa4 = reinterpret_cast<sockaddr_in *>(p->ai_addr);
          if (inet_ntop(sa4->sin_family, &sa4->sin_addr, buf, sizeof buf) ==
              nullptr) {
            log->error("inet_ntop failed: {}", strerror(errno));
            continue;
          }
          break;
        case AF_INET6:
#ifdef DISABLE_IPV6
          continue;
#else
          sa6 = reinterpret_cast<sockaddr_in6 *>(p->ai_addr);
          if (inet_ntop(sa6->sin6_family, &sa6->sin6_addr, buf, sizeof buf) ==
              nullptr) {
            log->error("inet_ntop failed: {}", strerror(errno));
            continue;
          }
          break;
#endif
        default:
          log->warn("unknown address family {}", p->ai_addr->sa_family);
          break;
      }
      addr.ip = buf;
      auto x = known_peers_.insert(addr);
      if (x.second) {
        log->debug("added peer {} (from seed {})", addr, seed);
      }
    }
    connect_to_peers();
  });
  request->nodeAddrInfo(seed);
}

void Client::connect_to_peers() {
  if (connections_.size() >= max_connections_) {
    return;
  }

  std::vector<uvw::Addr> peers(known_peers_.cbegin(), known_peers_.cend());
  shuffle(peers);
  while (connections_.size() < max_connections_ && !peers.empty()) {
    auto it = peers.begin();
    connect_to_peer(*it);
    if (known_peers_.erase(*it) != 1) {
      log->error("failed to erase known peer {}", *it);
    }
    peers.erase(it);
  }
}

void Client::connect_to_peer(const uvw::Addr &addr) {
  log->debug("connecting to {}", addr);

  auto loop = uvw::Loop::getDefault();
  auto conn = loop->resource<uvw::TcpHandle>();
  auto timer = loop->resource<uvw::TimerHandle>();

  conn->once<uvw::ErrorEvent>(
      [ this, addr, c = conn.get(), t = timer.get() ](const auto &, auto &) {
        log->warn("tcp error from {}", addr);
        if (!c->closing()) {
          remove_connection(c);
        }
        t->close();
        connect_to_peers();
      });
  conn->on<uvw::DataEvent>(
      [](const auto &, auto &) { log->info("got a data read event"); });
  conn->once<uvw::ConnectEvent>(
      [ addr, t = timer.get() ](const auto &, auto &) {
        log->info("connected to new peer {}", addr);
        t->close();
      });
  conn->connect(addr);
  connections_.push_back(conn);

  timer->on<uvw::TimerEvent>(
      [ this, addr, c = conn.get() ](const auto &, auto &timer) {
        log->warn("connection to {} timed out", addr);
        remove_connection(c);
        timer.close();
      });
  timer->start(CONNECT_TIMEOUT, NO_REPEAT);
}

void Client::remove_connection(uvw::TcpHandle *conn, bool reconnect) {
  bool removed = false;
  for (auto it = connections_.begin(); it != connections_.end(); it++) {
    if (it->get() == conn) {
      connections_.erase(it);
      removed = true;
      break;
    }
  }
  if (!removed) {
    auto addr = conn->data<uvw::Addr>();
    log->error("failed to remove connection to peer {}", *addr);
    return;
  }

  conn->close();
  if (reconnect) {
    connect_to_peers();
  }
}
}  // namespace spv
