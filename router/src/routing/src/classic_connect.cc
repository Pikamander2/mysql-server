/*
  Copyright (c) 2022, 2023, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "classic_connect.h"

#include <chrono>
#include <memory>

#include "basic_protocol_splicer.h"
#include "classic_connection_base.h"
#include "classic_frame.h"
#include "mysql/harness/logging/logging.h"
#include "mysql/harness/net_ts/impl/poll.h"
#include "mysql/harness/net_ts/internet.h"
#include "mysql/harness/stdx/expected.h"
#include "mysql/harness/utility/string.h"  // join
#include "mysqlrouter/connection_pool_component.h"
#include "mysqlrouter/datatypes.h"
#include "mysqlrouter/routing.h"
#include "mysqlrouter/routing_component.h"
#include "mysqlrouter/utils.h"  // to_string
#include "processor.h"

IMPORT_LOG_FUNCTIONS()

// create a destination id that's understood by make_tcp_address()
static std::string destination_id_from_endpoint(
    const std::string &host_name, const std::string &service_name) {
  if (net::ip::make_address_v6(host_name.c_str())) {
    return "[" + host_name + "]:" + service_name;
  } else {
    return host_name + ":" + service_name;
  }
}

static std::string destination_id_from_endpoint(
    const net::ip::tcp::resolver::results_type::iterator::value_type
        &endpoint) {
  return destination_id_from_endpoint(endpoint.host_name(),
                                      endpoint.service_name());
}

stdx::expected<Processor::Result, std::error_code> ConnectProcessor::process() {
  switch (stage()) {
    case Stage::InitDestination:
      return init_destination();
    case Stage::Resolve:
      return resolve();
    case Stage::InitEndpoint:
      return init_endpoint();
    case Stage::FromPool:
      return from_pool();
    case Stage::NextEndpoint:
      return next_endpoint();
    case Stage::NextDestination:
      return next_destination();
    case Stage::InitConnect:
      return init_connect();
    case Stage::Connect:
      return connect();
    case Stage::ConnectFinish:
      return connect_finish();
    case Stage::Connected:
      return connected();
    case Stage::Error:
      return error();
    case Stage::Done:
      return Result::Done;
  }

  harness_assert_this_should_not_execute();
}

static TlsSwitchableConnection make_connection_from_pooled(
    PooledClassicConnection &&other) {
  return {std::move(other.connection()),
          nullptr,  // routing_conn
          other.ssl_mode(), std::make_unique<Channel>(std::move(other.ssl())),
          std::make_unique<ClassicProtocolState>(
              other.server_capabilities(), other.client_capabilities(),
              other.server_greeting(), other.username(), other.schema(),
              other.attributes())};
}

// get the socket-error from a connection.
//
// error   if getting socket error failed.
// success if error could be fetched
static stdx::expected<std::error_code, std::error_code> sock_error_code(
    TlsSwitchableConnection &conn) {
  auto tcp_conn = dynamic_cast<TcpConnection *>(conn.connection().get());

  net::socket_base::error sock_err;
  const auto getopt_res = tcp_conn->get_option(sock_err);
  if (!getopt_res) return stdx::make_unexpected(getopt_res.error());

  if (sock_err.value() != 0) {
    return std::error_code {
      sock_err.value(),
#if defined(_WIN32)
          std::system_category()
#else
          std::generic_category()
#endif
    };
  }

  return {};
}

// skip destinations which don't matched the current expected server-mode.
static bool skip_destination(MysqlRoutingClassicConnectionBase *conn,
                             Destination *destination) {
  if (conn->context().access_mode() != routing::AccessMode::kAuto) return false;

  const auto conn_server_mode = conn->expected_server_mode();
  const auto dest_server_mode = destination->server_mode();

  return ((conn_server_mode == mysqlrouter::ServerMode::ReadOnly &&
           dest_server_mode == mysqlrouter::ServerMode::ReadWrite) ||
          (conn_server_mode == mysqlrouter::ServerMode::ReadWrite &&
           dest_server_mode == mysqlrouter::ServerMode::ReadOnly));
}

stdx::expected<Processor::Result, std::error_code>
ConnectProcessor::init_destination() {
  std::vector<std::string> dests;
  for (const auto &dest : destinations_) {
    dests.push_back(destination_id_from_endpoint(dest->hostname(),
                                                 std::to_string(dest->port())));
  }

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("connect::init_destination: " +
                                   mysql_harness::join(dests, ",")));
  }

  trace_event_connect_ =
      trace_span(parent_event_, "mysql/from_pool_or_connect");
  if (auto *ev = trace_event_connect_) {
    ev->attrs.emplace_back("mysql.remote.candidates",
                           mysql_harness::join(dests, ","));
  }

  // adjust the expected-server-mode depending if we have:
  //
  // - RW, RO
  // - only RW (multi-primary)
  // - only RO (replica of replicaset)
  if (connection()->context().access_mode() == routing::AccessMode::kAuto) {
    bool has_read_only{false};
    bool has_read_write{false};

    for (auto const &dest : destinations_) {
      if (dest->server_mode() == mysqlrouter::ServerMode::ReadOnly) {
        has_read_only = true;
      }
      if (dest->server_mode() == mysqlrouter::ServerMode::ReadWrite) {
        has_read_write = true;
      }
    }

    if (has_read_only && !has_read_write) {
      connection()->expected_server_mode(mysqlrouter::ServerMode::ReadOnly);
    } else if (!has_read_only && has_read_write) {
      connection()->expected_server_mode(mysqlrouter::ServerMode::ReadWrite);
    }
  }

  destinations_it_ = destinations_.begin();

  if (destinations_it_ != destinations_.end()) {
    const auto &destination = *destinations_it_;

    if (connection()->context().access_mode() == routing::AccessMode::kAuto) {
      if (skip_destination(connection(), destination.get())) {
        stage(Stage::NextDestination);

        return Result::Again;
      }
    }

    stage(is_destination_good(destination->hostname(), destination->port())
              ? Stage::Resolve
              : Stage::NextDestination);

    return Result::Again;
  } else {
    if (!last_ec_) {
      // no backends
      log_debug("init_destination(): the destinations list is empty");
      last_ec_ = make_error_code(DestinationsErrc::kNoDestinations);
    }

    stage(Stage::Error);
    return Result::Again;
  }
}

stdx::expected<Processor::Result, std::error_code> ConnectProcessor::resolve() {
  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("connect::resolve"));
  }

  const auto &destination = *destinations_it_;

  if (!destination->good()) {
    stage(Stage::NextDestination);

    return Result::Again;
  }

  if (!connection()->get_destination_id().empty()) {
    // already connected before. Make sure the same endpoint is connected.
    const auto dest_id = connection()->get_destination_id();

    if (auto &tr = tracer()) {
      tr.trace(Tracer::Event().stage("connect::sticky: " + dest_id));
    }

    if (dest_id !=
        destination_id_from_endpoint(destination->hostname(),
                                     std::to_string(destination->port()))) {
      stage(Stage::NextDestination);
      return Result::Again;
    }
  }

  const auto resolve_res = resolver_.resolve(
      destination->hostname(), std::to_string(destination->port()));

  if (!resolve_res) {
    log_debug("resolve(%s,%d) failed: %s:%s", destination->hostname().c_str(),
              destination->port(), resolve_res.error().category().name(),
              resolve_res.error().message().c_str());
    destination->connect_status(resolve_res.error());

    stage(Stage::NextDestination);
    return Result::Again;
  }

  endpoints_ = resolve_res.value();

#if 0
  std::cerr << __LINE__ << ": " << destination->hostname() << "\n";
  for (auto const &ep : endpoints_) {
    std::cerr << __LINE__ << ": .. " << ep.endpoint() << "\n";
  }
#endif

  stage(Stage::InitEndpoint);
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code>
ConnectProcessor::init_endpoint() {
  // trace(Tracer::Event().stage("connect::init_endpoint"));

  endpoints_it_ = endpoints_.begin();

  stage(Stage::InitConnect);
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code>
ConnectProcessor::init_connect() {
  // trace(Tracer::Event().stage("connect::init_connect"));

  auto tcp_conn = dynamic_cast<TcpConnection *>(
      connection()->socket_splicer()->server_conn().connection().get());

  // close socket if it is already open
  if (tcp_conn) (void)tcp_conn->close();

  connection()->connect_error_code({});  // reset the connect-error-code.

  auto endpoint = *endpoints_it_;

  server_endpoint_ = endpoint.endpoint();

  stage(Stage::FromPool);
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code>
ConnectProcessor::from_pool() {
  auto *socket_splicer = connection()->socket_splicer();
  auto *client_protocol = connection()->client_protocol();

  if (!client_protocol->client_greeting()) {
    // taking a connection from the pool requires that the client's greeting
    // must been received already.
    stage(Stage::Connect);
    return Result::Again;
  }

  trace_event_socket_from_pool_ =
      trace_span(trace_event_connect_, "mysql/from_pool");

  auto &pools = ConnectionPoolComponent::get_instance();

  if (auto pool = pools.get(ConnectionPoolComponent::default_pool_name())) {
    // pop the first connection from the pool that matches our requirements
    //
    // - endpoint
    // - capabilities

    auto client_caps = client_protocol->shared_capabilities();

    client_caps
        // connection specific.
        .reset(classic_protocol::capabilities::pos::ssl)
        .reset(classic_protocol::capabilities::pos::query_attributes)
        .reset(classic_protocol::capabilities::pos::compress)
        .reset(classic_protocol::capabilities::pos::compress_zstd)
        .reset(classic_protocol::capabilities::pos::session_track)
        .reset(classic_protocol::capabilities::pos::
                   text_result_with_session_tracking)
        // session specific capabilities which can be recovered by
        // set_server_option()
        .reset(classic_protocol::capabilities::pos::multi_statements);

    auto pool_res = pool->pop_if(
        [client_caps, ep = mysqlrouter::to_string(server_endpoint_),
         requires_tls = connection()->requires_tls(),
         requires_client_cert =
             connection()->requires_client_cert()](const auto &pooled_conn) {
          auto pooled_caps = pooled_conn.shared_capabilities();

          pooled_caps.reset(classic_protocol::capabilities::pos::ssl)
              .reset(classic_protocol::capabilities::pos::query_attributes)
              .reset(classic_protocol::capabilities::pos::compress)
              .reset(classic_protocol::capabilities::pos::compress_zstd)
              .reset(classic_protocol::capabilities::pos::session_track)
              .reset(classic_protocol::capabilities::pos::
                         text_result_with_session_tracking)
              .reset(classic_protocol::capabilities::pos::multi_statements);

          bool has_client_cert =
              pooled_conn.ssl() &&
              (SSL_get_certificate(pooled_conn.ssl().get()) != nullptr);

          return (pooled_conn.endpoint() == ep &&  //
                  client_caps == pooled_caps &&    //
                  (requires_tls == static_cast<bool>(pooled_conn.ssl())) &&
                  (requires_client_cert == has_client_cert));
        });

    if (pool_res) {
      // check if the socket is closed.
      std::array<net::impl::poll::poll_fd, 1> fds{
          {{pool_res->connection()->native_handle(), POLLIN, 0}}};
      auto poll_res = net::impl::poll::poll(fds.data(), fds.size(),
                                            std::chrono::milliseconds(0));
      if (!poll_res && poll_res.error() == std::errc::timed_out) {
        // nothing to read -> socket is still up.
        if (auto &tr = tracer()) {
          tr.trace(Tracer::Event().stage(
              "connect::from_pool: " +
              destination_id_from_endpoint(*endpoints_it_)));
        }

        // if the socket would be closed, recv() would return 0 for "eof".
        //
        // socket is still alive. good.
        socket_splicer->server_conn() =
            make_connection_from_pooled(std::move(*pool_res));

        (void)socket_splicer->server_conn().connection()->set_io_context(
            socket_splicer->client_conn().connection()->io_ctx());

        // reset the seq-id of the server side as this is a new command.
        if (connection()->server_protocol() != nullptr) {
          connection()->server_protocol()->seq_id(0xff);
        }

        if (connection()->expected_server_mode() ==
            mysqlrouter::ServerMode::Unavailable) {
          const auto *dest = destinations_it_->get();
          // before the first query, the server-mode is not set, remember it
          // now.
          connection()->expected_server_mode(dest->server_mode());
        }

        // set destination-id to get the "trace_set_connection_attributes"
        // right.
        connection()->destination_id(
            destination_id_from_endpoint(*endpoints_it_));

        if (auto *ev = trace_event_socket_from_pool_) {
          trace_set_connection_attributes(ev);
          trace_span_end(ev);
        }

        // update the msg-tracer callback to the new connection.
        if (auto *server_ssl = socket_splicer->server_channel()->ssl()) {
          SSL_set_msg_callback_arg(server_ssl, connection());
        }

        stage(Stage::Connected);
        return Result::Again;
      }

      // socket is dead. try the next one.
      return Result::Again;
    }

    if (auto *ev = trace_event_socket_from_pool_) {
      ev->attrs.emplace_back("mysql.error_message", "no match");
      trace_span_end(ev, TraceEvent::StatusCode::kError);
    }

  } else {
    if (auto *ev = trace_event_socket_from_pool_) {
      ev->attrs.emplace_back("mysql.error_message", "no pool");
      trace_span_end(ev, TraceEvent::StatusCode::kError);
    }
  }

  stage(Stage::Connect);
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code> ConnectProcessor::connect() {
  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("connect::connect: " +
                                   mysqlrouter::to_string(server_endpoint_)));
  }

  trace_event_socket_connect_ =
      trace_span(trace_event_connect_, "mysql/connect");

  if (auto *ev = trace_event_socket_connect_) {
    ev->attrs.emplace_back("net.peer.name", endpoints_it_->host_name());
    ev->attrs.emplace_back("net.peer.port", endpoints_it_->service_name());
  }

#if 0
  if (log_level_is_handled(mysql_harness::logging::LogLevel::kDebug)) {
    log_debug("trying %s", mysqlrouter::to_string(server_endpoint_).c_str());
  }
#endif

  const int socket_flags {
#if defined(SOCK_NONBLOCK)
    // linux|freebsd|sol11.4 allows to set NONBLOCK as part of the socket()
    // call to save the extra syscall
    SOCK_NONBLOCK
#endif
  };

  net::ip::tcp::socket server_sock(io_ctx_);

  auto open_res = server_sock.open(server_endpoint_.protocol(), socket_flags);
  if (!open_res) return open_res.get_unexpected();

  const auto non_block_res = server_sock.native_non_blocking(true);
  if (!non_block_res) return non_block_res.get_unexpected();

  server_sock.set_option(net::ip::tcp::no_delay{true});

#ifdef FUTURE_TASK_USE_SOURCE_ADDRESS
  /* set the source address to take a specific route.
   *
   *
   */

  // IP address of the interface we want to route-through.
  std::string src_addr_str;

  // src_addr_str = "192.168.178.78";

  if (!src_addr_str.empty()) {
    const auto src_addr_res = net::ip::make_address_v4(src_addr_str.c_str());
    if (!src_addr_res) return src_addr_res.get_unexpected();

#if defined(IP_BIND_ADDRESS_NO_PORT)
    // linux 4.2 introduced IP_BIND_ADDRESS_NO_PORT to delay assigning a
    // source-port until connect()
    net::socket_option::integer<IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT> sockopt;

    const auto setsockopt_res = server_sock.set_option(sockopt);
    if (!setsockopt_res) {
      // if the glibc supports IP_BIND_ADDRESS_NO_PORT, but the kernel
      // doesn't: ignore it.
      if (setsockopt_res.error() !=
          make_error_code(std::errc::invalid_argument)) {
        log_warning(
            "%d: setsockopt(IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT) "
            "failed: "
            "%s",
            __LINE__, setsockopt_res.error().message().c_str());
        return setsockopt_res.get_unexpected();
      }
    }
#endif

    const auto bind_res = server_sock.bind(net::ip::tcp::endpoint(
        src_addr_res.value_or(net::ip::address_v4{}), 0));
    if (!bind_res) return bind_res.get_unexpected();
  }
#endif

  const auto connect_res = server_sock.connect(server_endpoint_);

  // don't assign the connection if disconnect is requested.
  //
  // assigning the connection would lead to a deadlock in start_acceptor()
  auto disconnected_requested =
      connection()->disconnect_request([this, &server_sock](bool req) {
        if (req) return true;

        connection()->socket_splicer()->server_conn().assign_connection(
            std::make_unique<TcpConnection>(std::move(server_sock),
                                            server_endpoint_));

        return false;
      });
  if (disconnected_requested) {
    connection()->connect_error_code(
        make_error_code(std::errc::operation_canceled));

    stage(Stage::Done);
    return Result::Again;
  }

  if (!connect_res) {
    const auto ec = connect_res.error();
    if (ec == make_error_condition(std::errc::operation_in_progress) ||
        ec == make_error_condition(std::errc::operation_would_block)) {
      // connect in progress, wait for completion.
      stage(Stage::ConnectFinish);

      auto &t = connection()->connect_timer();

      t.expires_after(
          connection()->context().get_destination_connect_timeout());

      if (auto &tr = tracer()) {
        tr.trace(Tracer::Event().stage("connect::wait"));
      }
      t.async_wait([this](std::error_code ec) {
        if (ec) return;

        if (auto &tr = tracer()) {
          tr.trace(Tracer::Event().stage("connect::timed_out"));
        }

        auto *socket_splicer = connection()->socket_splicer();
        auto &server_conn = socket_splicer->server_conn();

        connection()->connect_error_code(make_error_code(std::errc::timed_out));

        (void)server_conn.cancel();
      });

      connection()->socket_splicer()->server_conn().async_wait_error(
          [conn = connection()](std::error_code ec) {
            if (ec) return;

            auto *socket_splicer = conn->socket_splicer();
            auto &server_conn = socket_splicer->server_conn();

            auto sock_ec_res = sock_error_code(server_conn);
            if (!sock_ec_res) {
              conn->connect_error_code(sock_ec_res.error());
            } else {
              conn->connect_error_code(sock_ec_res.value());
            }

            // cancel all the other waiters
            (void)server_conn.cancel();
          });

      return Result::SendableToServer;
    } else {
      log_debug("connect(%s, %d) failed: %s:%s",
                server_endpoint_.address().to_string().c_str(),
                server_endpoint_.port(), connect_res.error().category().name(),
                connect_res.error().message().c_str());
      connection()->connect_error_code(ec);

      stage(Stage::ConnectFinish);
      return Result::Again;
    }
  }

  stage(Stage::Connected);
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code>
ConnectProcessor::connect_finish() {
  connection()->connect_timer().cancel();

  auto &server_conn = connection()->socket_splicer()->server_conn();

  // cancel all handlers.
  (void)server_conn.cancel();

  if (connection()->connect_error_code() != std::error_code{}) {
    last_ec_ = connection()->connect_error_code();

    (void)server_conn.close();

    if (auto &tr = tracer()) {
      tr.trace(Tracer::Event().stage("connect::connect_finish: " +
                                     last_ec_.message()));
    }

    stage(Stage::NextEndpoint);
    return Result::Again;
  }

  auto sock_ec_res = sock_error_code(server_conn);
  if (!sock_ec_res) {
    last_ec_ = sock_ec_res.error();

    if (auto &tr = tracer()) {
      tr.trace(Tracer::Event().stage("connect::connect_finish: " +
                                     last_ec_.message()));
    }

    stage(Stage::NextEndpoint);
    return Result::Again;
  }

  auto sock_ec = *sock_ec_res;

  if (sock_ec != std::error_code{}) {
    last_ec_ = sock_ec;

    if (auto &tr = tracer()) {
      tr.trace(Tracer::Event().stage("connect::connect_finish: " +
                                     last_ec_.message()));
    }

    stage(Stage::NextEndpoint);
    return Result::Again;
  }

  if (auto *ev = trace_event_socket_connect_) {
    trace_span_end(ev);
  }

  stage(Stage::Connected);
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code>
ConnectProcessor::next_endpoint() {
  if (auto &tr = tracer()) {
    tr.trace(
        Tracer::Event().stage("connect::next_endpoint: " + last_ec_.message()));
  }

  if (auto *ev = trace_event_socket_connect_) {
    ev->attrs.emplace_back("mysql.error_message", last_ec_.message());

    trace_span_end(ev);
  }

  std::advance(endpoints_it_, 1);

  if (endpoints_it_ != endpoints_.end()) {
    stage(Stage::InitConnect);
    return Result::Again;
  } else {
    auto &destination = *destinations_it_;

    // report back the connect status to the destination
    destination->connect_status(last_ec_);

    if (last_ec_) {
      auto hostname = destination->hostname();
      auto port = destination->port();

      auto &ctx = connection()->context();

      if (ctx.shared_quarantine().update({hostname, port}, false)) {
        log_debug("[%s] add destination '%s:%d' to quarantine",
                  ctx.get_name().c_str(), hostname.c_str(), port);
      }
    }

    stage(Stage::NextDestination);
    return Result::Again;
  }
}

bool ConnectProcessor::is_destination_good(const std::string &hostname,
                                           uint16_t port) const {
  const auto &ctx = connection()->context();

  const auto is_quarantined =
      ctx.shared_quarantine().is_quarantined({hostname, port});
  if (is_quarantined) {
    log_debug("[%s] skip quarantined destination '%s:%d'",
              ctx.get_name().c_str(), hostname.c_str(), port);

    return false;
  }

  return true;
}

stdx::expected<Processor::Result, std::error_code>
ConnectProcessor::next_destination() {
  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("connect::next_destination"));
  }
  do {
    std::advance(destinations_it_, 1);

    if (destinations_it_ == std::end(destinations_)) break;

    const auto &destination = *destinations_it_;

    // for read-only connections, skip the writable destinations,
    // for read-write connections, skip the read-only destinations.
    if (skip_destination(connection(), destination.get())) continue;

    if (is_destination_good(destination->hostname(), destination->port())) {
      break;
    }
  } while (true);

  if (destinations_it_ != destinations_.end()) {
    // next destination
    stage(Stage::Resolve);
    return Result::Again;
  } else {
    auto refresh_res =
        connection()->destinations()->refresh_destinations(destinations_);
    if (refresh_res) {
      destinations_ = std::move(refresh_res.value());

      stage(Stage::InitDestination);
      return Result::Again;
    } else if (connection()->context().access_mode() ==
                   routing::AccessMode::kAuto &&
               connection()->expected_server_mode() ==
                   mysqlrouter::ServerMode::ReadOnly) {
      // if we want a RO connections but there are only primaries, take a
      // primary.
      connection()->expected_server_mode(mysqlrouter::ServerMode::ReadWrite);
      stage(Stage::InitDestination);
      return Result::Again;
    } else {
      // we couldn't connect to any of the destinations. Give up.
      stage(Stage::Error);
      return Result::Again;
    }
  }
}

stdx::expected<Processor::Result, std::error_code>
ConnectProcessor::connected() {
  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("connect::connected"));
  }

  if (auto *ev = trace_event_connect_) {
    trace_span_end(ev);
  }

  const auto *dest = destinations_it_->get();

  // remember the destination and its server-mode for connection-sharing.
  if (connection()->expected_server_mode() ==
      mysqlrouter::ServerMode::Unavailable) {
    // before the first query, the server-mode is not set, remember it now.
    connection()->expected_server_mode(dest->server_mode());
  }

  connection()->destination_id(destination_id_from_endpoint(*endpoints_it_));

  // mark destination as reachable.
  connection()->context().shared_quarantine().update(
      {dest->hostname(), dest->port()}, true);

  // back to the caller.
  stage(Stage::Done);
  return Result::Again;
}

stdx::expected<Processor::Result, std::error_code> ConnectProcessor::error() {
  auto *tcp_conn = dynamic_cast<TcpConnection *>(
      connection()->socket_splicer()->server_conn().connection().get());

  // close socket if it is already open
  if (tcp_conn != nullptr) (void)tcp_conn->close();

  if (auto &tr = tracer()) {
    tr.trace(Tracer::Event().stage("connect::error"));
  }

  const auto ec = last_ec_;

  connection()->connect_error_code(ec);

  log_debug("ConnectProcessor::error(): %s:%s", ec.category().name(),
            ec.message().c_str());

  if (ec == DestinationsErrc::kNoDestinations) {
    log_error("no backend available to connect to");
  } else {
    log_fatal_error_code("connecting to backend failed", ec);
  }

  if (auto *ev = trace_event_connect_) {
    ev->attrs.emplace_back(
        "mysql.error_message",
        ec == DestinationsErrc::kNoDestinations ? "no backend" : ec.message());
    trace_span_end(ev);
  }

  if (ec == make_error_condition(std::errc::too_many_files_open) ||
      ec == make_error_condition(std::errc::too_many_files_open_in_system)) {
    // release file-descriptors on the connection pool when out-of-fds is
    // noticed.
    //
    // don't retry as router may run into an infinite loop.
    ConnectionPoolComponent::get_instance().clear();
  } else if (ec == DestinationsErrc::kNoDestinations &&
             connection()->get_destination_id().empty()) {
    // if there are no destinations for a fresh connect, close the
    // acceptor-ports
    //
    // fresh-connect == "destiantion-id is empty"
    if (auto &tr = tracer()) {
      tr.trace(Tracer::Event().stage("connect::error::all_down"));
    }
    // all backends are down.
    MySQLRoutingComponent::get_instance()
        .api(connection()->context().get_id())
        .stop_socket_acceptors();
  }

  connection()->client_greeting_sent(true);
  connection()->authenticated(false);

  stage(Stage::Done);

  on_error_({2003, "Can't connect to remote MySQL server", "HY000"});

  return Result::Again;
}
