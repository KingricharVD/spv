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

#include <cassert>

#include "./logging.h"
#include "./uvw.h"

namespace spv {
MODULE_LOGGER

static const std::chrono::seconds HEADER_TIMEOUT{19};
static const std::chrono::seconds NO_REPEAT{0};

// copied from chainparams.cpp
static const std::vector<std::string> testSeeds = {
    "testnet-seed.bitcoin.jonasschnelli.ch", "seed.tbtc.petertodd.org",
    "testnet-seed.bluematt.me",
};

Client::Client(const Settings &settings, std::shared_ptr<uvw::Loop> loop)
    : settings_(settings),
      shutdown_(false),
      need_headers_(true),
      chain_(settings.datadir),
      us_(rand64(), 0, settings.version, settings.user_agent),
      loop_(loop) {}

void Client::run() {
  log->debug("connecting to network as {}", us_.user_agent);
  for (const auto &seed : testSeeds) {
    lookup_seed(seed);
  }
}

void Client::lookup_seed(const std::string &seed) {
  auto request = loop_->resource<uvw::GetAddrInfoReq>();
  request->on<uvw::ErrorEvent>([=](const auto &, auto &req) {
    log->warn("async dns resolution to {} failed", seed);
  });
  request->on<uvw::AddrInfoEvent>([=](const auto &event, auto &req) {
    for (const addrinfo *p = event.data.get(); p != nullptr; p = p->ai_next) {
      seed_peers_.emplace(p);
    }
    connect_to_addr(select_peer());
    remove_dns_request(&req);
  });
  request->nodeAddrInfo(seed);
  dns_requests_.push_back(request);
}

Addr Client::select_peer() const {
  // list of peers we are already connected to
  std::unordered_set<Addr> connections;
  for (const auto &pr : connections_) {
    connections.insert(pr.first);
  }

  // first try to get a peer from the regular list
  std::vector<Addr> peers;
  for (const auto &peer : peers_) {
    if (connections.find(peer.addr) == connections.end()) {
      peers.push_back(peer.addr);
    }
  }
  if (peers.size()) {
    auto it = random_choice(peers.begin(), peers.end());
    log->debug("select_peer() choosing peer {} from peers", *it);
    return *it;
  }

  // otherwise use the seed peer list
  for (const auto &peer : seed_peers_) {
    if (connections.find(peer) == connections.end()) {
      peers.push_back(peer);
    }
  }
  assert(peers.size());
  auto it = random_choice(peers.begin(), peers.end());
  log->debug("select_peer() choosing peer {} from seed peers", *it);
  return *it;
}

bool Client::is_connected_to_addr(const Addr &addr) const {
  for (const auto &pr : connections_) {
    if (pr.first == addr) return true;
  }
  return false;
}

void Client::connect_to_addr(const Addr &addr) {
  log->debug("connecting to peer {}", addr);

  Connection *conn = new Connection(this, addr);
  auto pr = connections_.insert(std::make_pair(addr, conn));
  assert(pr.second);

  auto timer = loop_->resource<uvw::TimerHandle>();
  auto weak_timer = timer->weak_from_this();
  auto cancel_timer = [=]() {
    if (auto t = weak_timer.lock()) t->close();
  };

  conn->tcp_->once<uvw::ErrorEvent>([=](const auto &exc, auto &c) {
    if (exc.code() == ECONNREFUSED) {
      log->debug("peer {} refused our TCP request", conn->peer());
    } else {
      log->warn("error from peer {}: {} {} {}", conn->peer(), exc.what(),
                exc.name(), exc.code());
    }
    cancel_timer();
    remove_connection(conn);
  });
  conn->tcp_->on<uvw::DataEvent>([=](const auto &data, auto &) {
    conn->read(data.data.get(), data.length);
  });
  conn->tcp_->once<uvw::CloseEvent>([=](const auto &, auto &tcp) {
    log->info("close event for connection {}", addr);
    cancel_timer();
    connect_to_new_peer();
  });
  conn->tcp_->once<uvw::ConnectEvent>([=](const auto &, auto &) {
    log->info("connected to new peer {}, connections = {}", addr,
              connections_.size());
    cancel_timer();
    conn->tcp_->read();
    conn->send_version();
  });
  conn->tcp_->once<uvw::EndEvent>([=](const auto &, auto &c) {
    log->info("remote peer {} closed connection", addr);
    remove_connection(conn);
  });
  conn->connect();

  timer->once<uvw::ErrorEvent>(
      [=](const auto &, auto &timer) { timer.close(); });
  timer->on<uvw::TimerEvent>([=](const auto &, auto &timer) {
    log->warn("connection to {} timed out", conn->peer());
    remove_connection(conn);
    timer.close();
  });
  timer->start(std::chrono::seconds(1), NO_REPEAT);
}

void Client::connect_to_new_peer() {
  if (!shutdown_ && connections_.size() < settings_.max_connections) {
    connect_to_addr(select_peer());
  }
}

size_t Client::get_height() const { return chain_.height(); }

void Client::remove_connection(Connection *conn) {
  const Addr &addr = conn->peer().addr;
  log->warn("removing connection to {}", conn->peer());
  auto it = connections_.find(addr);
  if (it == connections_.end()) {
    log->warn("connection {} was already removed", addr);
    return;
  }

  bool erased = false;
  for (auto it = peers_.begin(); it != peers_.end(); ++it) {
    if (it->addr == conn->peer().addr) {
      peers_.erase(it);
      erased = true;
      break;
    }
  }
  if (!erased && !shutdown_) {
    log->error("failed to remove peer {} after error", conn->peer());
  }

  // TODO: double check that the conn destructor actually shuts down its
  // resources properly.
  connections_.erase(it);
}

void Client::shutdown() {
  if (!shutdown_) {
    log->info("shutting down client");
    shutdown_ = true;

    for (auto &pr : connections_) {
      pr.second->shutdown();
    }
    cancel_hdr_timeout();
    cancel_dns_requests();
  }
}

void Client::notify_connected(Connection *conn) {
  if (need_headers_ && !hdr_timeout_) {
    log->info("starting header download");
    sync_more_headers(conn);
  }
}

void Client::notify_peer(Connection *conn, const NetAddr &addr) {
  auto pr = peers_.insert(addr);
  if (pr.second) {
    log->info("added new peer {}, peer list size {}", addr, peers_.size());
    if (connections_.size() < settings_.max_connections &&
        !is_connected_to_addr(addr)) {
      connect_to_addr(addr.addr);
    }
  } else {
    log->debug("ignoring duplicate peer {}", addr);
  }
}

void Client::notify_error(Connection *conn, const std::string &why) {
  log->warn("error on connection to {}, reason: {}", conn->peer(), why);
  remove_connection(conn);
}

void Client::sync_more_headers(Connection *conn) {
  if (conn == nullptr) {
    conn = random_connection();
    assert(conn != nullptr);
  }
  auto peer = conn->peer();  // captured by value
  assert(!hdr_timeout_);
  hdr_timeout_ = loop_->resource<uvw::TimerHandle>();
  hdr_timeout_->once<uvw::ErrorEvent>([this](const auto &, auto &timer) {
    log->error("got error from header timer");
    hdr_timeout_.reset();
    timer.close();
  });
  hdr_timeout_->on<uvw::TimerEvent>([this, peer](const auto &, auto &timer) {
    log->warn("get headers timeout from peer {}", peer);
    timer.close();
    hdr_timeout_.reset();
    sync_more_headers();
  });
  hdr_timeout_->start(HEADER_TIMEOUT, NO_REPEAT);
  conn->get_headers(chain_.tip());
}

void Client::notify_headers(Connection *conn,
                            const std::vector<BlockHeader> &block_headers) {
  cancel_hdr_timeout();
  if (block_headers.empty() && chain_.tip_is_recent()) {
    log->info("header syncing finished, tip is {}", chain_.tip());
    need_headers_ = false;
    return;
  }
  if (block_headers.size() < 2000) {
    log->warn("got {} new headers, last is {}", block_headers.size(),
              *block_headers.end());
  }

  for (const auto &hdr : block_headers) {
    chain_.put_block_header(hdr);

    Inv inv(InvType::BLOCK, hdr.block_hash);
    auto pos = pending_inv_.find(inv);
    if (pos != pending_inv_.end()) {
      log->debug("de-queueing inv");
      pending_inv_.erase(pos);
    }
  }
  chain_.save_tip();
  log->info("saved chain tip {} via peer {}", chain_.tip(), conn->peer());
  sync_more_headers();
}

bool Client::need_inv(const Inv &inv) const {
  // are we already trying to get this block?
  if (pending_inv_.find(inv) != pending_inv_.end()) {
    return false;
  }
  return !chain_.has_block(inv.hash);
}

void Client::notify_inv(Connection *conn, const Inv &inv) {
  if (need_inv(inv)) {
    log->warn("fetching new inv {} {}", to_string(inv.type), to_hex(inv.hash));
    pending_inv_.insert(inv);
    log->debug("added inv, pending list = {}", pending_inv_.size());
    conn->get_data(inv);
  } else {
    log->debug("skipping duplicate inv");
  }
}

Connection *Client::random_connection() {
  std::vector<Connection *> conns;
  for (auto &c : connections_) {
    if (c.second->connected()) {
      conns.push_back(c.second.get());
    }
  }
  if (conns.empty()) {
    log->warn("no connected peers, return nullptr from random_connection()");
    return nullptr;
  }
  return *random_choice(conns.begin(), conns.end());
}

void Client::cancel_hdr_timeout() {
  if (hdr_timeout_) {
    hdr_timeout_->stop();
    hdr_timeout_->close();
    hdr_timeout_.reset();
  }
}

void Client::cancel_dns_requests() {
  for (auto &sp : dns_requests_) {
    sp->cancel();
  }
  dns_requests_.clear();
}

void Client::remove_dns_request(uvw::GetAddrInfoReq *req) {
  auto p = std::find_if(dns_requests_.begin(), dns_requests_.end(),
                        [=](const auto &p) { return p.get() == req; });
  if (p == dns_requests_.end()) {
    log->warn("cannot cancel dns request {}, it is not in request list",
              (void *)req);
    return;
  }
  (*p)->cancel();
  dns_requests_.erase(p);
}
}  // namespace spv
