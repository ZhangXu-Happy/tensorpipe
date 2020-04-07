/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <tensorpipe/channel/cma/cma.h>

#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <limits>

#include <tensorpipe/channel/error.h>
#include <tensorpipe/channel/helpers.h>
#include <tensorpipe/common/callback.h>
#include <tensorpipe/common/defs.h>
#include <tensorpipe/common/error_macros.h>
#include <tensorpipe/common/system.h>

namespace tensorpipe {
namespace channel {
namespace cma {

namespace {

const std::string kChannelName{"cma"};

std::string generateDomainDescriptor() {
  std::ostringstream oss;
  auto bootID = getBootID();
  TP_THROW_ASSERT_IF(!bootID) << "Unable to read boot_id";

  // According to the man page of process_vm_readv and process_vm_writev,
  // permission to read from or write to another process is governed by a ptrace
  // access mode PTRACE_MODE_ATTACH_REALCREDS check. This consists in a series
  // of checks, some governed by the CAP_SYS_PTRACE capability, others by the
  // Linux Security Modules (LSMs), but the primary constraint is that the real,
  // effective, and saved-set user IDs of the target match the caller's real
  // user ID, and the same for group IDs. Since channels are bidirectional, we
  // end up needing these IDs to all be the same on both processes.

  // Combine boot ID, effective UID, and effective GID.
  oss << kChannelName;
  oss << ":" << bootID.value();
  // FIXME As domain descriptors are just compared for equality, we only include
  // the effective IDs, but we should abide by the rules above and make sure
  // that they match the real and saved-set ones too.
  oss << "/" << geteuid();
  oss << "/" << getegid();
  return oss.str();
}

} // namespace

Context::Context()
    : channel::Context(kChannelName), impl_(std::make_shared<Impl>()) {}

Context::Impl::Impl()
    : domainDescriptor_(generateDomainDescriptor()), requests_(INT_MAX) {
  thread_ = std::thread(&Impl::handleCopyRequests_, this);
}

void Context::close() {
  impl_->close();
}

void Context::Impl::close() {
  // FIXME Acquiring this lock causes a deadlock when calling join. The solution
  // is avoiding locks by using the event loop approach just like in transports.
  // std::unique_lock<std::mutex> lock(mutex_);

  bool wasClosed = false;
  closed_.compare_exchange_strong(wasClosed, true);
  if (!wasClosed) {
    closingEmitter_.close();
    requests_.push(nullopt);
  }
}

void Context::join() {
  impl_->join();
}

void Context::Impl::join() {
  std::unique_lock<std::mutex> lock(mutex_);

  close();

  bool wasJoined = false;
  joined_.compare_exchange_strong(wasJoined, true);
  if (!wasJoined) {
    thread_.join();
    // TP_DCHECK(requests_.empty());
  }
}

Context::~Context() {
  join();
}

ClosingEmitter& Context::Impl::getClosingEmitter() {
  return closingEmitter_;
}

const std::string& Context::domainDescriptor() const {
  return impl_->domainDescriptor();
}

const std::string& Context::Impl::domainDescriptor() const {
  std::unique_lock<std::mutex> lock(mutex_);
  return domainDescriptor_;
}

std::shared_ptr<channel::Channel> Context::createChannel(
    std::shared_ptr<transport::Connection> connection,
    Channel::Endpoint endpoint) {
  return impl_->createChannel(std::move(connection), endpoint);
}

std::shared_ptr<channel::Channel> Context::Impl::createChannel(
    std::shared_ptr<transport::Connection> connection,
    Channel::Endpoint /* unused */) {
  TP_THROW_ASSERT_IF(joined_);
  return std::make_shared<Channel>(
      Channel::ConstructorToken(),
      std::static_pointer_cast<PrivateIface>(shared_from_this()),
      std::move(connection));
}

void Context::Impl::requestCopy(
    pid_t remotePid,
    void* remotePtr,
    void* localPtr,
    size_t length,
    std::function<void(const Error&)> fn) {
  requests_.push(
      CopyRequest{remotePid, remotePtr, localPtr, length, std::move(fn)});
}

void Context::Impl::handleCopyRequests_() {
  while (true) {
    auto maybeRequest = requests_.pop();
    if (!maybeRequest.has_value()) {
      break;
    }
    CopyRequest request = std::move(maybeRequest).value();

    // Perform copy.
    struct iovec local {
      .iov_base = request.localPtr, .iov_len = request.length
    };
    struct iovec remote {
      .iov_base = request.remotePtr, .iov_len = request.length
    };
    auto nread =
        ::process_vm_readv(request.remotePid, &local, 1, &remote, 1, 0);
    if (nread == -1) {
      request.callback(TP_CREATE_ERROR(SystemError, "cma", errno));
    } else if (nread != request.length) {
      request.callback(TP_CREATE_ERROR(ShortReadError, request.length, nread));
    } else {
      request.callback(Error::kSuccess);
    }
  }
}

Channel::Channel(
    ConstructorToken /* unused */,
    std::shared_ptr<Context::PrivateIface> context,
    std::shared_ptr<transport::Connection> connection)
    : impl_(Impl::create(std::move(context), std::move(connection))) {}

std::shared_ptr<Channel::Impl> Channel::Impl::create(
    std::shared_ptr<Context::PrivateIface> context,
    std::shared_ptr<transport::Connection> connection) {
  auto impl = std::make_shared<Impl>(
      ConstructorToken(), std::move(context), std::move(connection));
  impl->init_();
  return impl;
}

Channel::Impl::Impl(
    ConstructorToken /* unused */,
    std::shared_ptr<Context::PrivateIface> context,
    std::shared_ptr<transport::Connection> connection)
    : context_(std::move(context)),
      connection_(std::move(connection)),
      closingReceiver_(context_, context_->getClosingEmitter()) {}

void Channel::Impl::init_() {
  deferToLoop_([this]() { initFromLoop_(); });
}

void Channel::Impl::initFromLoop_() {
  TP_DCHECK(inLoop_());
  closingReceiver_.activate(*this);
  readPacket_();
}

bool Channel::Impl::inLoop_() {
  return currentLoop_ == std::this_thread::get_id();
}

void Channel::Impl::deferToLoop_(std::function<void()> fn) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    pendingTasks_.push_back(std::move(fn));
    if (currentLoop_ != std::thread::id()) {
      return;
    }
    currentLoop_ = std::this_thread::get_id();
  }

  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (pendingTasks_.empty()) {
        currentLoop_ = std::thread::id();
        return;
      }
      task = std::move(pendingTasks_.front());
      pendingTasks_.pop_front();
    }
    task();
  }
}

void Channel::send(
    const void* ptr,
    size_t length,
    TDescriptorCallback descriptorCallback,
    TSendCallback callback) {
  impl_->send(ptr, length, std::move(descriptorCallback), std::move(callback));
}

void Channel::Impl::send(
    const void* ptr,
    size_t length,
    TDescriptorCallback descriptorCallback,
    TSendCallback callback) {
  deferToLoop_([this,
                ptr,
                length,
                descriptorCallback{std::move(descriptorCallback)},
                callback{std::move(callback)}]() mutable {
    sendFromLoop_(
        ptr, length, std::move(descriptorCallback), std::move(callback));
  });
}

void Channel::Impl::sendFromLoop_(
    const void* ptr,
    size_t length,
    TDescriptorCallback descriptorCallback,
    TSendCallback callback) {
  TP_DCHECK(inLoop_());
  // TP_THROW_ASSERT_IF(context_->joined_);
  if (error_) {
    // FIXME Ideally here we should either call the callback with an error (but
    // this may deadlock if we do it inline) or return an error as an additional
    // return value.
    TP_THROW_ASSERT();
  }
  proto::Descriptor pbDescriptor;

  const auto id = id_++;
  pbDescriptor.set_operation_id(id);
  pbDescriptor.set_pid(getpid());
  pbDescriptor.set_ptr(reinterpret_cast<uint64_t>(ptr));
  sendOperations_.emplace_back(SendOperation{id, std::move(callback)});

  descriptorCallback(Error::kSuccess, saveDescriptor(pbDescriptor));
}

// Receive memory region from peer.
void Channel::recv(
    TDescriptor descriptor,
    void* ptr,
    size_t length,
    TRecvCallback callback) {
  impl_->recv(std::move(descriptor), ptr, length, std::move(callback));
}

void Channel::Impl::recv(
    TDescriptor descriptor,
    void* ptr,
    size_t length,
    TRecvCallback callback) {
  deferToLoop_([this,
                descriptor{std::move(descriptor)},
                ptr,
                length,
                callback{std::move(callback)}]() mutable {
    recvFromLoop_(std::move(descriptor), ptr, length, std::move(callback));
  });
}

void Channel::Impl::recvFromLoop_(
    TDescriptor descriptor,
    void* ptr,
    size_t length,
    TRecvCallback callback) {
  TP_DCHECK(inLoop_());
  // TODO Short cut this if we're already in an error state.
  const auto pbDescriptor = loadDescriptor<proto::Descriptor>(descriptor);
  const uint64_t id = pbDescriptor.operation_id();
  pid_t remotePid = pbDescriptor.pid();
  void* remotePtr = reinterpret_cast<void*>(pbDescriptor.ptr());

  context_->requestCopy(
      remotePid,
      remotePtr,
      ptr,
      length,
      copyCallbackWrapper_([id, callback{std::move(callback)}](Impl& impl) {
        // Let peer know we've completed the copy.
        auto pbPacketOut = std::make_shared<proto::Packet>();
        proto::Notification* pbNotification =
            pbPacketOut->mutable_notification();
        pbNotification->set_operation_id(id);
        impl.connection_->write(
            *pbPacketOut,
            impl.writePacketCallbackWrapper_(
                [pbPacketOut](Impl& /* unused */) {}));
        callback(impl.error_);
      }));
}

void Channel::close() {
  impl_->close();
}

Channel::~Channel() {
  close();
}

void Channel::Impl::close() {
  deferToLoop_([this]() { closeFromLoop_(); });
}

void Channel::Impl::closeFromLoop_() {
  TP_DCHECK(inLoop_());
  if (!error_) {
    error_ = TP_CREATE_ERROR(ChannelClosedError);
    handleError_();
  }
}

void Channel::Impl::readPacket_() {
  TP_DCHECK(inLoop_());
  auto pbPacketIn = std::make_shared<proto::Packet>();
  connection_->read(
      *pbPacketIn, readPacketCallbackWrapper_([pbPacketIn](Impl& impl) {
        impl.onPacket_(*pbPacketIn);
      }));
}

void Channel::Impl::onPacket_(const proto::Packet& pbPacketIn) {
  TP_DCHECK(inLoop_());

  TP_DCHECK_EQ(pbPacketIn.type_case(), proto::Packet::kNotification);
  onNotification_(pbPacketIn.notification());

  // Arm connection to wait for next packet.
  readPacket_();
}

void Channel::Impl::onNotification_(const proto::Notification& pbNotification) {
  TP_DCHECK(inLoop_());

  // Find the send operation matching the notification's operation ID.
  const auto id = pbNotification.operation_id();
  auto it = std::find_if(
      sendOperations_.begin(), sendOperations_.end(), [id](const auto& op) {
        return op.id == id;
      });
  TP_THROW_ASSERT_IF(it == sendOperations_.end())
      << "Expected send operation with ID " << id << " to exist.";

  // Move operation to stack.
  auto op = std::move(*it);
  sendOperations_.erase(it);

  // Execute send completion callback.
  op.callback(Error::kSuccess);
}

void Channel::Impl::handleError_() {
  TP_DCHECK(inLoop_());

  // Move pending operations to stack.
  auto sendOperations = std::move(sendOperations_);

  // Notify pending send callbacks of error.
  for (auto& op : sendOperations) {
    op.callback(error_);
  }

  connection_->close();
}

} // namespace cma
} // namespace channel
} // namespace tensorpipe
