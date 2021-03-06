#include "gtest/gtest.h"

#include "ATen/cuda/CUDAContext.h"
#include "ATen/cuda/CUDAGuard.h"
#include "ATen/cuda/CUDAEvent.h"

#include "cuda_runtime.h"

#include <functional>
#include <thread>
#include <unordered_set>

#define ASSERT_EQ_CUDA(X, Y) \
  {                          \
    bool isTRUE = X == Y;    \
    ASSERT_TRUE(isTRUE);     \
  }

#define ASSERT_NE_CUDA(X, Y) \
  {                          \
    bool isFALSE = X == Y;   \
    ASSERT_FALSE(isFALSE);   \
  }

/*
   Tests related to ATen streams.
   */
// Verifies streams are live through copying and moving
TEST(TestStream, CopyAndMoveTest) {
  int32_t device = -1;
  cudaStream_t cuda_stream;

  // Tests that copying works as expected and preserves the stream
  at::cuda::CUDAStream copyStream;
  {
    auto s = at::cuda::createCUDAStream();
    device = s.device();
    cuda_stream = s.stream();

    copyStream = s;

    ASSERT_EQ_CUDA(copyStream.internals(), s.internals());
    ASSERT_EQ_CUDA(copyStream.device(), device);
    ASSERT_EQ_CUDA(copyStream.stream(), cuda_stream);
  }

  ASSERT_TRUE(copyStream.internals());
  ASSERT_EQ_CUDA(copyStream.device(), device);
  ASSERT_EQ_CUDA(copyStream.stream(), cuda_stream);

  // Tests that moving works as expected and preserves the stream
  at::cuda::CUDAStream moveStream;
  {
    auto s = at::cuda::createCUDAStream();
    device = s.device();
    cuda_stream = s.stream();

    moveStream = std::move(s);

    ASSERT_EQ_CUDA(moveStream.device(), device);
    ASSERT_EQ_CUDA(moveStream.stream(), cuda_stream);
  }

  ASSERT_TRUE(moveStream.internals());
  ASSERT_EQ_CUDA(moveStream.device(), device);
  ASSERT_EQ_CUDA(moveStream.stream(), cuda_stream);
}

// Verifies streams are set properly
TEST(TestStream, GetAndSetTest) {
  at::cuda::CUDAStream myStream = at::cuda::createCUDAStream();

  // Sets and gets
  at::cuda::setCurrentCUDAStream(myStream);
  at::cuda::CUDAStream curStream = at::cuda::getCurrentCUDAStream();

  ASSERT_EQ_CUDA(myStream, curStream);

  // Gets, sets, and gets default stream
  at::cuda::CUDAStream defaultStream = at::cuda::getDefaultCUDAStream();
  at::cuda::setCurrentCUDAStream(defaultStream);
  curStream = at::cuda::getCurrentCUDAStream();

  ASSERT_NE_CUDA(defaultStream, myStream);
  ASSERT_EQ_CUDA(curStream, defaultStream);
}

void thread_fun(at::cuda::CUDAStream& cur_thread_stream) {
  auto new_stream = at::cuda::createCUDAStream();
  at::cuda::setCurrentCUDAStream(new_stream);
  cur_thread_stream = at::cuda::getCurrentCUDAStream();
  ASSERT_EQ_CUDA(cur_thread_stream, new_stream);
}

// Ensures streams are thread local
TEST(TestStream, MultithreadGetAndSetTest) {
  at::cuda::CUDAStream s0, s1;

  std::thread t0{thread_fun, std::ref(s0)};
  std::thread t1{thread_fun, std::ref(s1)};
  t0.join();
  t1.join();

  at::cuda::CUDAStream cur_stream = at::cuda::getCurrentCUDAStream();
  at::cuda::CUDAStream default_stream = at::cuda::getDefaultCUDAStream();

  ASSERT_EQ_CUDA(cur_stream, default_stream);
  ASSERT_NE_CUDA(cur_stream, s0);
  ASSERT_NE_CUDA(cur_stream, s1);
  ASSERT_NE_CUDA(s0, s1);
}

// CUDA Guard
TEST(TestStream, CUDAGuardTest) {
  if (at::cuda::getNumGPUs() < 2) {
    return;
  }

  // -- begin setup

  ASSERT_EQ_CUDA(at::cuda::current_device(), 0);
  std::vector<at::cuda::CUDAStream> streams0 = {
      at::cuda::getDefaultCUDAStream(), at::cuda::createCUDAStream()};
  ASSERT_EQ_CUDA(streams0[0].device(), 0);
  ASSERT_EQ_CUDA(streams0[1].device(), 0);
  at::cuda::setCurrentCUDAStream(streams0[0]);

  std::vector<at::cuda::CUDAStream> streams1;
  {
    at::DeviceGuard device_guard(1);
    streams1.push_back(at::cuda::getDefaultCUDAStream());
    streams1.push_back(at::cuda::createCUDAStream());
  }
  ASSERT_EQ_CUDA(streams1[0].device(), 1);
  ASSERT_EQ_CUDA(streams1[1].device(), 1);
  at::cuda::setCurrentCUDAStream(streams1[0]);

  ASSERT_EQ_CUDA(at::cuda::current_device(), 0);

  // -- end setup

  // Test that all original streams are recorded.
  {
    at::cuda::CUDAGuard guard;
    ASSERT_TRUE(guard.original_streams().empty());
    guard.set_stream(streams0[0]);
    ASSERT_EQ_CUDA(guard.original_streams().size(), at::cuda::getNumGPUs());
    ASSERT_EQ_CUDA(guard.original_streams()[0], streams0[0]);
    ASSERT_EQ_CUDA(guard.original_streams()[1], streams1[0]);
  }

  // Setting a stream changes the current device and the stream on that device
  {
    at::cuda::CUDAGuard guard(streams1[1]);
    ASSERT_EQ_CUDA(guard.last_device(), 1);
    ASSERT_EQ_CUDA(at::cuda::current_device(), 1);
    ASSERT_EQ_CUDA(at::cuda::getCurrentCUDAStream(1), streams1[1]);
  }

  // Device and stream are now reset
  ASSERT_EQ_CUDA(at::cuda::current_device(), 0);
  ASSERT_EQ_CUDA(at::cuda::getCurrentCUDAStream(1), streams1[0]);

  // Setting only the device changes only the current device and not the stream
  {
    at::cuda::CUDAGuard guard(/*device=*/1);
    ASSERT_EQ_CUDA(guard.last_device(), 1);
    ASSERT_EQ_CUDA(at::cuda::current_device(), 1);
    ASSERT_EQ_CUDA(at::cuda::getCurrentCUDAStream(1), streams1[0]);
  }

  ASSERT_EQ_CUDA(at::cuda::current_device(), 0);
  ASSERT_EQ_CUDA(at::cuda::getCurrentCUDAStream(0), streams0[0]);

  // Setting the stream first, and then the device, first changes the devices
  // back, and then resets the stream on the initial device.

  {
    at::cuda::CUDAGuard guard(streams0[1]);
    guard.set_device(1);
  }

  ASSERT_EQ_CUDA(at::cuda::current_device(), 0);
  ASSERT_EQ_CUDA(at::cuda::getCurrentCUDAStream(0), streams0[0]);
  ASSERT_EQ_CUDA(at::cuda::getCurrentCUDAStream(1), streams1[0]);
}

// CUDAGuardIsMovable
TEST(TestStream, CUDAGuardMovableTest) {
  if (at::cuda::getNumGPUs() < 2) {
    return;
  }
  const auto stream = at::cuda::createCUDAStream();
  const auto device_count = at::cuda::getNumGPUs();
  at::cuda::CUDAGuard first(stream);
  first.set_device(1);
  at::cuda::CUDAGuard second(std::move(first));
  ASSERT_EQ_CUDA(second.original_streams().size(), device_count);
  ASSERT_EQ_CUDA(second.original_device(), 0);
  ASSERT_EQ_CUDA(second.last_device(), 1);
  at::cuda::CUDAGuard third;
  third = std::move(second);
  ASSERT_EQ_CUDA(third.original_streams().size(), device_count);
  ASSERT_EQ_CUDA(third.original_device(), 0);
  ASSERT_EQ_CUDA(third.last_device(), 1);
}

// Streampool Round Robin
TEST(TestStream, StreamPoolTest) {
  std::vector<at::cuda::CUDAStream> streams{};
  for (int i = 0; i < 200; ++i) {
    streams.emplace_back(at::cuda::detail::CUDAStream_createStream());
  }

  std::unordered_set<cudaStream_t> stream_set{};
  bool hasDuplicates = false;
  for (auto i = decltype(streams.size()){0}; i < streams.size(); ++i) {
    cudaStream_t cuda_stream = streams[i];
    auto result_pair = stream_set.insert(cuda_stream);
    if (!result_pair.second)
      hasDuplicates = true;
  }

  ASSERT_TRUE(hasDuplicates);
}

// Multi-GPU
TEST(TestStream, MultiGPUTest) {
  if (at::cuda::getNumGPUs() < 2)
    return;

  at::cuda::CUDAStream s0 = at::cuda::createCUDAStream(true, 0);
  at::cuda::CUDAStream s1 = at::cuda::createCUDAStream(false, 1);

  at::cuda::setCurrentCUDAStream(s0);
  at::cuda::setCurrentCUDAStream(s1);

  ASSERT_EQ_CUDA(s0, at::cuda::getCurrentCUDAStream());

  at::DeviceGuard device_guard{1};
  ASSERT_EQ_CUDA(s1, at::cuda::getCurrentCUDAStream());
}

// CUDAEvent Syncs
TEST(TestStream, CUDAEventSyncTest) {
  const auto stream = at::cuda::createCUDAStream();
  at::cuda::CUDAEvent event;

  ASSERT_FALSE(event.happened());

  event.recordOnce(stream);

  const auto wait_stream0 = at::cuda::createCUDAStream();
  const auto wait_stream1 = at::cuda::createCUDAStream();

  wait_stream0.synchronize_with(event);
  wait_stream1.synchronize_with(event);

  cudaStreamSynchronize(wait_stream0);
  ASSERT_TRUE(event.happened());
}

// Cross-Device Events
TEST(TestStream, CrossDeviceTest) {
  if (at::cuda::getNumGPUs() < 2)
    return;

  const auto stream0 = at::cuda::createCUDAStream();
  at::cuda::CUDAEvent event0;

  at::cuda::set_device(1);
  const auto stream1 = at::cuda::createCUDAStream();
  at::cuda::CUDAEvent event1;

  event0.record(stream0);
  event1.record(stream1);

  event0 = std::move(event1);

  ASSERT_EQ_CUDA(event0.device(), 1);

  stream0.synchronize_with(event0);

  cudaStreamSynchronize(stream0);
  ASSERT_TRUE(event0.happened());
}
