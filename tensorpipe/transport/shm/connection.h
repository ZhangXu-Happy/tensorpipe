/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include <tensorpipe/transport/connection.h>
#include <tensorpipe/transport/defs.h>

namespace tensorpipe {
namespace transport {
namespace shm {

class Context;
class Listener;
class Loop;
class Socket;

class Connection final : public transport::Connection {
  // Use the passkey idiom to allow make_shared to call what should be a private
  // constructor. See https://abseil.io/tips/134 for more information.
  struct ConstructorToken {};

 public:
  // Create a connection that is already connected (e.g. from a listener).
  Connection(
      ConstructorToken,
      std::shared_ptr<Loop> loop,
      std::shared_ptr<Socket> socket);

  // Create a connection that connects to the specified address.
  Connection(ConstructorToken, std::shared_ptr<Loop> loop, address_t addr);

  // Implementation of transport::Connection.
  void read(read_callback_fn fn) override;

  // Implementation of transport::Connection.
  void read(google::protobuf::MessageLite& message, read_proto_callback_fn fn)
      override;

  // Implementation of transport::Connection.
  void read(void* ptr, size_t length, read_callback_fn fn) override;

  // Implementation of transport::Connection
  void write(const void* ptr, size_t length, write_callback_fn fn) override;

  // Implementation of transport::Connection
  void write(const google::protobuf::MessageLite& message, write_callback_fn fn)
      override;

  // Close connection.
  void close() override;

  ~Connection() override;

 private:
  class Impl;

  std::shared_ptr<Loop> loop_;
  std::shared_ptr<Impl> impl_;

  // Allow context to access constructor token.
  friend class Context;
  // Allow listener to access constructor token.
  friend class Listener;
};

} // namespace shm
} // namespace transport
} // namespace tensorpipe
