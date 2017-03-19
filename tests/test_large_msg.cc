#include <gtest/gtest.h>
#include <string.h>
#include <atomic>
#include <thread>
#include "rpc.h"
#include "test_printf.h"
#include "util/rand.h"

using namespace ERpc;

static constexpr uint16_t kAppNexusUdpPort = 31851;
static constexpr double kAppNexusPktDropProb = 0.0;
static constexpr size_t kAppEventLoopMs = 200;
static constexpr size_t kAppMaxEventLoopMs = 10000; /* 10 seconds */
static constexpr uint8_t kAppClientAppTid = 100;
static constexpr uint8_t kAppServerAppTid = 200;
static constexpr uint8_t kAppReqType = 3;
static constexpr size_t kAppMinMsgSize =
    Rpc<IBTransport>::max_data_per_pkt() + 1; /* At least 2 packets */

/* Shared between client and server thread */
std::atomic<bool> server_ready; /* Client starts after server is ready */
std::atomic<bool> client_done;  /* Server ends after client is done */

const uint8_t phy_port = 0;
const size_t numa_node = 0;
char local_hostname[kMaxHostnameLen];

/// Per-thread application context
struct app_context_t {
  bool is_client;
  Rpc<IBTransport> *rpc;
  FastRand fastrand;

  size_t num_sm_connect_resps = 0; /* Client-only */
  size_t num_rpc_resps = 0;        /* Client-only */
};

size_t pick_large_msg_size(app_context_t *app_context) {
  assert(app_context != nullptr);
  uint32_t sample = app_context->fastrand.next_u32();
  uint32_t msg_size =
      sample % (Rpc<IBTransport>::kMaxMsgSize - kAppMinMsgSize) +
      kAppMinMsgSize;

  assert(msg_size >= kAppMinMsgSize &&
         msg_size <= Rpc<IBTransport>::kMaxMsgSize);
  return (size_t)msg_size;
}

/// The common request handler for all subtests. Copies the request string to
/// the response.
void req_handler(const MsgBuffer *req_msgbuf, app_resp_t *app_resp,
                 void *_context) {
  ASSERT_NE(req_msgbuf, nullptr);
  ASSERT_NE(app_resp, nullptr);
  ASSERT_NE(_context, nullptr);

  auto *context = (app_context_t *)_context;
  ASSERT_FALSE(context->is_client);

  size_t req_size = req_msgbuf->get_data_size();
  test_printf("Server: Received request of length %zu\n", req_size);

  app_resp->prealloc_used = false;
  app_resp->resp_size = req_size;
  app_resp->dyn_resp_msgbuf = context->rpc->alloc_msg_buffer(req_size);
  ASSERT_NE(app_resp->dyn_resp_msgbuf.buf, nullptr);

  memcpy((char *)app_resp->dyn_resp_msgbuf.buf, (char *)req_msgbuf->buf,
         req_size);
  app_resp->resp_size = req_size;
}

/// The common response handler for all subtests. This checks that the request
/// buffer is identical to the response buffer, and increments the number of
/// responses in the context.
void resp_handler(const MsgBuffer *req_msgbuf, const MsgBuffer *resp_msgbuf,
                  void *_context) {
  ASSERT_NE(req_msgbuf, nullptr);
  ASSERT_NE(resp_msgbuf, nullptr);
  ASSERT_NE(_context, nullptr);

  test_printf("Client: Received response of length %zu (request's was %zu)\n",
              (char *)resp_msgbuf->get_data_size(),
              (char *)req_msgbuf->get_data_size());

  ASSERT_EQ(req_msgbuf->get_data_size(), resp_msgbuf->get_data_size());
  ASSERT_STREQ((char *)req_msgbuf->buf, (char *)resp_msgbuf->buf);

  auto *context = (app_context_t *)_context;
  ASSERT_TRUE(context->is_client);
  context->num_rpc_resps++;
}

void sm_hander(Session *session, SessionMgmtEventType sm_event_type,
               SessionMgmtErrType sm_err_type, void *_context) {
  _unused(session);

  auto *context = (app_context_t *)_context;
  ASSERT_TRUE(context->is_client);
  context->num_sm_connect_resps++;

  ASSERT_EQ(sm_err_type, SessionMgmtErrType::kNoError);
  ASSERT_TRUE(sm_event_type == SessionMgmtEventType::kConnected ||
              sm_event_type == SessionMgmtEventType::kDisconnected);
}

/// The server thread used for all subtests
void server_thread_func(Nexus *nexus, uint8_t app_tid) {
  app_context_t context;
  context.is_client = false;

  Rpc<IBTransport> rpc(nexus, (void *)&context, app_tid, &sm_hander, phy_port,
                       numa_node);
  rpc.register_ops(kAppReqType, Ops(req_handler, resp_handler));
  context.rpc = &rpc;
  server_ready = true;

  while (!client_done) { /* Wait for the client */
    rpc.run_event_loop_timeout(kAppEventLoopMs);
  }

  /* The client is done after disconnecting */
  ASSERT_EQ(rpc.num_active_sessions(), 0);
}

/// Test: Send one large request message and check that we receive the
/// correct response
void one_large_rpc(Nexus *nexus) {
  while (!server_ready) { /* Wait for server */
    usleep(1);
  }

  volatile app_context_t context;
  context.is_client = true;

  Rpc<IBTransport> rpc(nexus, (void *)&context, kAppClientAppTid, &sm_hander,
                       phy_port, numa_node);
  rpc.register_ops(kAppReqType, Ops(req_handler, resp_handler));

  context.rpc = &rpc;

  /* Connect the session */
  Session *session =
      rpc.create_session(local_hostname, kAppServerAppTid, phy_port);

  while (context.num_sm_connect_resps == 0) {
    rpc.run_event_loop_one();
  }
  ASSERT_EQ(context.num_sm_connect_resps, 1);
  ASSERT_EQ(session->state, SessionState::kConnected);

  /* Send a message */
  size_t req_size = kAppMinMsgSize;
  MsgBuffer req_msgbuf = rpc.alloc_msg_buffer(req_size);
  ASSERT_NE(req_msgbuf.buf, nullptr);

  for (size_t i = 0; i < req_size; i++) {
    req_msgbuf.buf[i] = 'a';
  }
  req_msgbuf.buf[req_size - 1] = 0;

  test_printf("test: Sending request of size %zu\n", req_size);
  int ret = rpc.send_request(session, kAppReqType, &req_msgbuf);
  if (ret != 0) {
    test_printf("test: send_request error %s\n",
                rpc.rpc_datapath_err_code_str(ret).c_str());
  }
  ASSERT_EQ(ret, 0);

  /* Run the event loop for up to kAppMaxEventLoopMs milliseconds */
  uint64_t cycles_start = rdtsc();
  while (context.num_rpc_resps != 1) {
    rpc.run_event_loop_timeout(kAppEventLoopMs);

    double ms_elapsed = to_msec(rdtsc() - cycles_start, nexus->freq_ghz);
    if (ms_elapsed > kAppMaxEventLoopMs) {
      break;
    }
  }

  ASSERT_EQ(context.num_rpc_resps, 1);

  rpc.free_msg_buffer(req_msgbuf);

  /* Disconnect the session */
  rpc.destroy_session(session);
  rpc.run_event_loop_timeout(kAppEventLoopMs);

  client_done = true;
}

TEST(OneLargeRpc, OneLargeRpc) {
  Nexus nexus(kAppNexusUdpPort, kAppNexusPktDropProb);
  server_ready = false;
  client_done = false;

  std::thread server_thread(server_thread_func, &nexus, kAppServerAppTid);
  std::thread client_thread(one_large_rpc, &nexus);
  server_thread.join();
  client_thread.join();
}

/// Test: Repeat: Multiple large Rpcs on one session, with random size
void multi_large_rpc_one_session(Nexus *nexus) {
  while (!server_ready) { /* Wait for server */
    usleep(1);
  }

  volatile app_context_t context;
  context.is_client = true;

  Rpc<IBTransport> rpc(nexus, (void *)&context, kAppClientAppTid, &sm_hander,
                       phy_port, numa_node);
  rpc.register_ops(kAppReqType, Ops(req_handler, resp_handler));

  context.rpc = &rpc;

  /* Connect the session */
  Session *session =
      rpc.create_session(local_hostname, kAppServerAppTid, phy_port);

  while (context.num_sm_connect_resps == 0) {
    rpc.run_event_loop_one();
  }
  ASSERT_EQ(context.num_sm_connect_resps, 1);
  ASSERT_EQ(session->state, SessionState::kConnected);

  /* Pre-create MsgBuffers so we can test reuse and resizing */
  MsgBuffer req_msgbuf[Session::kSessionCredits];
  for (size_t i = 0; i < Session::kSessionCredits; i++) {
    req_msgbuf[i] = rpc.alloc_msg_buffer(Rpc<IBTransport>::kMaxMsgSize);
  }

  for (size_t iter = 0; iter < 2; iter++) {
    context.num_rpc_resps = 0;

    /* Enqueue as many requests as one session allows */
    for (size_t i = 0; i < Session::kSessionCredits; i++) {
      size_t req_len = pick_large_msg_size((app_context_t *)&context);
      rpc.resize_msg_buffer(&req_msgbuf[i], req_len);

      for (size_t j = 0; j < req_len; j++) {
        req_msgbuf[i].buf[j] = 'a' + ((i + j) % 26);
      }
      req_msgbuf[i].buf[req_len - 1] = 0;

      test_printf("test: Sending request of length = %zu\n", req_len);
      int ret = rpc.send_request(session, kAppReqType, &req_msgbuf[i]);
      if (ret != 0) {
        test_printf("test: send_request error %s\n",
                    rpc.rpc_datapath_err_code_str(ret).c_str());
      }
      ASSERT_EQ(ret, 0);
    }

    /* Try to enqueue one more request - this should fail */
    int ret = rpc.send_request(session, kAppReqType, &req_msgbuf[0]);
    ASSERT_NE(ret, 0);

    /* Run the event loop for up to kAppMaxEventLoopMs milliseconds */
    uint64_t cycles_start = rdtsc();
    while (context.num_rpc_resps != Session::kSessionCredits) {
      rpc.run_event_loop_timeout(kAppEventLoopMs);

      double ms_elapsed = to_msec(rdtsc() - cycles_start, nexus->freq_ghz);
      if (ms_elapsed > kAppMaxEventLoopMs) {
        break;
      }
    }

    ASSERT_EQ(context.num_rpc_resps, Session::kSessionCredits);
  }

  /* Free the request MsgBuffers */
  for (size_t i = 0; i < Session::kSessionCredits; i++) {
    rpc.free_msg_buffer(req_msgbuf[i]);
  }

  /* Disconnect the session */
  rpc.destroy_session(session);
  rpc.run_event_loop_timeout(kAppEventLoopMs);

  client_done = true;
}

TEST(MultiLargeRpcOneSession, MultiLargeRpcOneSession) {
  Nexus nexus(kAppNexusUdpPort, kAppNexusPktDropProb);
  server_ready = false;
  client_done = false;

  std::thread server_thread(server_thread_func, &nexus, kAppServerAppTid);
  std::thread client_thread(multi_large_rpc_one_session, &nexus);
  server_thread.join();
  client_thread.join();
}

/// Test: Repeat: Multiple large Rpcs on multiple sessions
void multi_large_rpc_multi_session(Nexus *nexus, size_t num_sessions) {
  while (!server_ready) { /* Wait for server */
    usleep(1);
  }

  volatile app_context_t context;
  context.is_client = true;

  Rpc<IBTransport> rpc(nexus, (void *)&context, kAppClientAppTid, &sm_hander,
                       phy_port, numa_node);
  rpc.register_ops(kAppReqType, Ops(req_handler, resp_handler));

  context.rpc = &rpc;

  /* Connect the sessions */
  Session *session[num_sessions];
  for (size_t sess_i = 0; sess_i < num_sessions; sess_i++) {
    session[sess_i] = rpc.create_session(
        local_hostname, kAppServerAppTid + (uint8_t)sess_i, phy_port);
  }

  while (context.num_sm_connect_resps < num_sessions) {
    rpc.run_event_loop_one();
  }
  ASSERT_EQ(context.num_sm_connect_resps, num_sessions);

  for (size_t sess_i = 0; sess_i < num_sessions; sess_i++) {
    ASSERT_EQ(session[sess_i]->state, SessionState::kConnected);
  }

  /* Pre-create MsgBuffers so we can test reuse and resizing */
  size_t tot_reqs_per_iter = num_sessions * Session::kSessionCredits;
  MsgBuffer req_msgbuf[tot_reqs_per_iter];
  for (size_t req_i = 0; req_i < tot_reqs_per_iter; req_i++) {
    req_msgbuf[req_i] = rpc.alloc_msg_buffer(Rpc<IBTransport>::kMaxMsgSize);
    ASSERT_NE(req_msgbuf[req_i].buf, nullptr);
  }

  for (size_t iter = 0; iter < 5; iter++) {
    context.num_rpc_resps = 0;

    for (size_t sess_i = 0; sess_i < num_sessions; sess_i++) {
      /* Enqueue as many requests as this session allows */
      for (size_t crd_i = 0; crd_i < Session::kSessionCredits; crd_i++) {
        size_t req_i = (sess_i * Session::kSessionCredits) + crd_i;
        assert(req_i < tot_reqs_per_iter);

        size_t req_len = pick_large_msg_size((app_context_t *)&context);
        rpc.resize_msg_buffer(&req_msgbuf[req_i], req_len);

        for (size_t j = 0; j < req_len; j++) {
          req_msgbuf[req_i].buf[j] = 'a' + ((req_i + j) % 26);
        }
        req_msgbuf[req_i].buf[req_len - 1] = 0;

        test_printf("test: Sending request of length = %zu\n", req_len);

        int ret =
            rpc.send_request(session[sess_i], kAppReqType, &req_msgbuf[req_i]);
        if (ret != 0) {
          test_printf("test: send_request error %s\n",
                      rpc.rpc_datapath_err_code_str(ret).c_str());
        }
        ASSERT_EQ(ret, 0);
      }

      /* Try to enqueue one more request - this should fail */
      int ret = rpc.send_request(session[sess_i], kAppReqType, &req_msgbuf[0]);
      ASSERT_NE(ret, 0);
    }

    /* Run the event loop for up to kAppMaxEventLoopMs milliseconds */
    uint64_t cycles_start = rdtsc();
    while (context.num_rpc_resps != tot_reqs_per_iter) {
      rpc.run_event_loop_timeout(kAppEventLoopMs);

      double ms_elapsed = to_msec(rdtsc() - cycles_start, nexus->freq_ghz);
      if (ms_elapsed > kAppMaxEventLoopMs) {
        break;
      }
    }
    ASSERT_EQ(context.num_rpc_resps, tot_reqs_per_iter);
  }

  /* Free the request MsgBuffers */
  for (size_t req_i = 0; req_i < tot_reqs_per_iter; req_i++) {
    rpc.free_msg_buffer(req_msgbuf[req_i]);
  }

  /* Disconnect the sessions */
  for (size_t sess_i = 0; sess_i < num_sessions; sess_i++) {
    rpc.destroy_session(session[sess_i]);
  }

  rpc.run_event_loop_timeout(kAppEventLoopMs);

  client_done = true;
}

TEST(MultiLargeRpcMultiSession, MultiLargeRpcMultiSession) {
  Nexus nexus(kAppNexusUdpPort, kAppNexusPktDropProb);
  server_ready = false;
  client_done = false;

  /* Use enough sessions to exceed the Rpc's unexpected window */
  size_t num_sessions =
      (Rpc<IBTransport>::kRpcUnexpPktWindow / Session::kSessionCredits) + 2;

  test_printf("test: Using %zu sessions\n", num_sessions);

  std::thread server_thread[num_sessions];

  /* Launch one server Rpc thread for each client session */
  for (size_t i = 0; i < num_sessions; i++) {
    server_thread[i] =
        std::thread(server_thread_func, &nexus, kAppServerAppTid + i);
  }

  std::thread client_thread(multi_large_rpc_multi_session, &nexus,
                            num_sessions);

  for (size_t i = 0; i < num_sessions; i++) {
    server_thread[i].join();
  }

  client_thread.join();
}

int main(int argc, char **argv) {
  Nexus::get_hostname(local_hostname);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}