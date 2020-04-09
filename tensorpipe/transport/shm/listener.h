/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include <tensorpipe/transport/listener.h>

namespace tensorpipe {
namespace transport {
namespace shm {

class Context;
class Loop;
class Sockaddr;

class Listener final : public transport::Listener {
  // Use the passkey idiom to allow make_shared to call what should be a private
  // constructor. See https://abseil.io/tips/134 for more information.
  struct ConstructorToken {};

 public:
  // Create a listener that listens on the specified address.
  Listener(ConstructorToken, std::shared_ptr<Loop> loop, address_t addr);

  using transport::Listener::accept_callback_fn;

  void accept(accept_callback_fn fn) override;

  address_t addr() const override;

  void close() override;

  ~Listener() override;

 private:
  class Impl;

  std::shared_ptr<Loop> loop_;
  std::shared_ptr<Impl> impl_;

  // Allow context to access constructor token.
  friend class Context;
};

} // namespace shm
} // namespace transport
} // namespace tensorpipe
