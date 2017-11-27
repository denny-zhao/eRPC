#include "system_tests.h"

namespace erpc {

static constexpr size_t kTestSmallMsgSize = 32;

class TestContext {
 public:
  Rpc<TestTransport> *rpc = nullptr;
  size_t num_req_handler_calls = 0;
  size_t num_cont_func_calls = 0;
};

/// The common request handler for subtests. Copies request to response.
void req_handler(ReqHandle *req_handle, void *_context) {
  auto *context = static_cast<TestContext *>(_context);
  const MsgBuffer *req_msgbuf = req_handle->get_req_msgbuf();
  const size_t resp_size = req_msgbuf->get_data_size();

  req_handle->prealloc_used = true;
  context->rpc->resize_msg_buffer(&req_handle->pre_resp_msgbuf, resp_size);
  memcpy(req_handle->pre_resp_msgbuf.buf, req_msgbuf->buf, resp_size);

  context->rpc->enqueue_response(req_handle);
  context->num_req_handler_calls++;
}

/// The common continuation for subtests.
void cont_func(RespHandle *resp_handle, void *_context, size_t) {
  auto *context = static_cast<TestContext *>(_context);
  context->num_cont_func_calls++;
  context->rpc->release_response(resp_handle);
}

class RpcRxTest : public RpcTest {
 public:
  RpcRxTest() {
    // Set Rpc context
    test_context.rpc = rpc;
    test_context.rpc->set_context(&test_context);
  }

  TestContext test_context;
};

//
// process_small_req_st()
//
TEST_F(RpcRxTest, process_small_req_st) {
  const auto server = get_local_endpoint();
  const auto client = get_remote_endpoint();

  create_server_session_init(client, server);
  Session *srv_session = rpc->session_vec[0];
  rpc->transport->resolve_remote_routing_info(
      &srv_session->client.routing_info);
  SSlot *sslot = &srv_session->sslot_arr[0];

  // The request packet that is recevied
  MsgBuffer req = rpc->alloc_msg_buffer(kTestSmallMsgSize);
  pkthdr_t *pkthdr_0 = req.get_pkthdr_0();
  pkthdr_0->req_type = kTestReqType;
  pkthdr_0->msg_size = kTestSmallMsgSize;
  pkthdr_0->dest_session_num = server.session_num;
  pkthdr_0->pkt_type = kPktTypeReq;
  pkthdr_0->pkt_num = 0;
  pkthdr_0->req_num = Session::kSessionReqWindow;

  // In-order: Receive an in-order small request.
  // Response handler is called and response is sent.
  rpc->process_small_req_st(sslot, pkthdr_0);
  ASSERT_EQ(test_context.num_req_handler_calls, 1);
  ASSERT_EQ(rpc->testing.pkthdr_tx_queue.pop().pkt_type, PktType::kPktTypeResp);
  test_context.num_req_handler_calls = 0;

  // Duplicate: Receive the same request again.
  // Request handler is not called. Response is re-sent, and TX queue flushed.
  rpc->process_small_req_st(sslot, pkthdr_0);
  ASSERT_EQ(test_context.num_req_handler_calls, 0);
  ASSERT_EQ(rpc->testing.pkthdr_tx_queue.pop().pkt_type, PktType::kPktTypeResp);
  ASSERT_EQ(rpc->transport->testing.tx_flush_count, 1);

  // Duplicate: Receive the same request again, but response is not ready yet.
  // Request handler is not called and response is not re-sent.
  MsgBuffer *tx_msgbuf_save = sslot->tx_msgbuf;
  sslot->tx_msgbuf = nullptr;
  rpc->process_small_req_st(sslot, pkthdr_0);
  ASSERT_EQ(test_context.num_req_handler_calls, 0);
  sslot->tx_msgbuf = tx_msgbuf_save;

  // Past: Receive an old request.
  // Request handler is not called and response is not re-sent.
  sslot->cur_req_num += Session::kSessionReqWindow;
  rpc->process_small_req_st(sslot, pkthdr_0);
  ASSERT_EQ(test_context.num_req_handler_calls, 0);
  ASSERT_EQ(rpc->testing.pkthdr_tx_queue.size(), 0);
  sslot->cur_req_num -= Session::kSessionReqWindow;

  // In-order: Receive the next in-order request.
  // Response handler is called and response is sent.
  pkthdr_0->req_num += Session::kSessionReqWindow;
  rpc->process_small_req_st(sslot, pkthdr_0);
  ASSERT_EQ(test_context.num_req_handler_calls, 1);
  ASSERT_EQ(rpc->testing.pkthdr_tx_queue.pop().pkt_type, PktType::kPktTypeResp);
  test_context.num_req_handler_calls = 0;

  // Future: Receive a future request packet. This is an error.
  pkthdr_0->req_num += 2 * Session::kSessionReqWindow;
  ASSERT_DEATH(rpc->process_small_req_st(sslot, pkthdr_0), ".*");
}

//
// process_small_resp_st() with a single-packet request
//
TEST_F(RpcRxTest, process_small_resp_st_small_req) {
  const auto client = get_local_endpoint();
  const auto server = get_remote_endpoint();

  create_client_session_init(client, server);
  Session *clt_session = rpc->session_vec[0];
  clt_session->server.session_num = server.session_num;
  rpc->transport->resolve_remote_routing_info(
      &clt_session->server.routing_info);
  clt_session->state = SessionState::kConnected;

  MsgBuffer req = rpc->alloc_msg_buffer(kTestSmallMsgSize);
  MsgBuffer local_resp = rpc->alloc_msg_buffer(kTestSmallMsgSize);

  // Use enqueue_request() to do sslot formatting for the request
  rpc->enqueue_request(0, kTestReqType, &req, &local_resp, cont_func, 0);
  SSlot *sslot_0 = &clt_session->sslot_arr[0];

  // Construct the basic test response packet
  MsgBuffer remote_resp = rpc->alloc_msg_buffer(kTestSmallMsgSize);
  pkthdr_t *pkthdr_0 = remote_resp.get_pkthdr_0();
  pkthdr_0->req_type = kTestReqType;
  pkthdr_0->msg_size = kTestSmallMsgSize;
  pkthdr_0->dest_session_num = client.session_num;
  pkthdr_0->pkt_type = kPktTypeResp;
  pkthdr_0->pkt_num = 0;
  pkthdr_0->req_num = Session::kSessionReqWindow;

  // In-order: Receive an in-order small response.
  // Continuation is invoked.
  rpc->process_small_resp_st(sslot_0, pkthdr_0);
  ASSERT_EQ(test_context.num_cont_func_calls, 1);
  ASSERT_EQ(sslot_0->tx_msgbuf, nullptr);  // Response received
  test_context.num_cont_func_calls = 0;

  // Duplicate: Receive the same response again.
  // It's ignored.
  rpc->process_small_resp_st(sslot_0, pkthdr_0);
  ASSERT_EQ(test_context.num_cont_func_calls, 0);

  // Past: Receive an old response.
  // It's ignored.
  sslot_0->cur_req_num += Session::kSessionReqWindow;
  rpc->process_small_resp_st(sslot_0, pkthdr_0);
  ASSERT_EQ(test_context.num_cont_func_calls, 0);
  sslot_0->cur_req_num -= Session::kSessionReqWindow;

  // Future: Receive a future response packet.
  // This is an error.
  pkthdr_0->req_num += Session::kSessionReqWindow;
  ASSERT_DEATH(rpc->process_small_resp_st(sslot_0, pkthdr_0), ".*");
}

//
// process_small_resp_st() with a multi-packet request
//
TEST_F(RpcRxTest, process_small_resp_st_large_req) {
  const size_t kTestLargeReqPkts = 2;
  const auto client = get_local_endpoint();
  const auto server = get_remote_endpoint();

  create_client_session_init(client, server);
  Session *clt_session = rpc->session_vec[0];
  clt_session->server.session_num = server.session_num;
  rpc->transport->resolve_remote_routing_info(
      &clt_session->server.routing_info);
  clt_session->state = SessionState::kConnected;

  MsgBuffer req =
      rpc->alloc_msg_buffer(rpc->get_max_data_per_pkt() * kTestLargeReqPkts);
  MsgBuffer local_resp = rpc->alloc_msg_buffer(kTestSmallMsgSize);

  // Use enqueue_request() to do sslot formatting for the request
  rpc->enqueue_request(0, kTestReqType, &req, &local_resp, cont_func, 0);
  SSlot *sslot_0 = &clt_session->sslot_arr[0];

  // Construct the basic test response packet
  MsgBuffer remote_resp = rpc->alloc_msg_buffer(kTestSmallMsgSize);
  pkthdr_t *pkthdr_0 = remote_resp.get_pkthdr_0();
  pkthdr_0->req_type = kTestReqType;
  pkthdr_0->msg_size = kTestSmallMsgSize;
  pkthdr_0->dest_session_num = client.session_num;
  pkthdr_0->pkt_type = kPktTypeResp;
  pkthdr_0->pkt_num = 0;
  pkthdr_0->req_num = Session::kSessionReqWindow;

  // In-order: Receive response in-order
  // Contination is invoked.
  sslot_0->client_info.req_sent = kTestLargeReqPkts;
  sslot_0->client_info.expl_cr_rcvd = kTestLargeReqPkts - 1;
  clt_session->client_info.credits -= 1;
  rpc->process_small_resp_st(sslot_0, pkthdr_0);
  ASSERT_EQ(test_context.num_cont_func_calls, 1);
  ASSERT_EQ(sslot_0->tx_msgbuf, nullptr);  // Response received
  test_context.num_cont_func_calls = 0;
}

}  // End erpc

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
