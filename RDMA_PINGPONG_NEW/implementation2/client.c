#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "messages.h"

struct client_context
{
  char *buffer;
  struct ibv_mr *buffer_mr;

  struct message *msg;
  struct ibv_mr *msg_mr;

  uint64_t peer_addr;
  uint32_t peer_rkey;
};


static void write_remote(struct rdma_cm_id *id, uint32_t len)
{
  struct client_context *ctx = (struct client_context *)id->context;
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));
  wr.wr_id = (uintptr_t)id;
  wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
  wr.imm_data = htonl(len);
  wr.wr.rdma.remote_addr = ctx->peer_addr;
  wr.wr.rdma.rkey = ctx->peer_rkey;

  if (len) {
    wr.sg_list = &sge;
    wr.num_sge = 1;
    sge.addr = (uintptr_t)ctx->buffer;
    sge.length = len;
    sge.lkey = ctx->buffer_mr->lkey;
  }
  ibv_post_send(id->qp, &wr, &bad_wr);
}

static void post_receive(struct rdma_cm_id *id)
{
  struct client_context *ctx = (struct client_context *)id->context;
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = (uintptr_t)id;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  sge.addr = (uintptr_t)ctx->msg;
  sge.length = sizeof(*ctx->msg);
  sge.lkey = ctx->msg_mr->lkey;
  TEST_NZ(ibv_post_recv(id->qp, &wr, &bad_wr));
}

static void send_file_name(struct rdma_cm_id *id){
  struct client_context *ctx = (struct client_context *)id->context;
  strcpy(ctx->buffer, "chara");
  write_remote(id, strlen(ctx->buffer) + 1);
}

static void on_pre_conn(struct rdma_cm_id *id)
{
  struct client_context *ctx = (struct client_context *)id->context;
  posix_memalign((void **)&ctx->buffer, sysconf(_SC_PAGESIZE), BUFFER_SIZE);
  TEST_Z(ctx->buffer_mr = ibv_reg_mr(rc_get_pd(), ctx->buffer, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE));
  posix_memalign((void **)&ctx->msg, sysconf(_SC_PAGESIZE), sizeof(*ctx->msg));
  TEST_Z(ctx->msg_mr = ibv_reg_mr(rc_get_pd(), ctx->msg, sizeof(*ctx->msg), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));
  // posix_memalign((void **)&ctx->msg->buffer, sysconf(_SC_PAGESIZE), BUFFER_SIZE);
  // TEST_Z(ctx->msg->buffer_mr = ibv_reg_mr(rc_get_pd(), ctx->msg->buffer, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));
  post_receive(id);
}

static void on_completion(struct ibv_wc *wc)
{
  struct rdma_cm_id *id = (struct rdma_cm_id *)(uintptr_t)(wc->wr_id);
  struct client_context *ctx = (struct client_context *)id->context;

  if (wc->opcode & IBV_WC_RECV) {
    if (ctx->msg->id == MSG_MR) {
      ctx->peer_addr = ctx->msg->data.mr.addr;
      ctx->peer_rkey = ctx->msg->data.mr.rkey;
      printf("received msg: %s\n", ctx->msg->buffer);
      send_file_name(id);
    } else if (ctx->msg->id == MSG_DONE) {
      printf("received done: disconnecting\n");
      rc_disconnect(id);
      return;
    }
    post_receive(id);
  }
}

int main(int argc, char **argv){
  struct client_context ctx;
  if (argc != 2) {
    fprintf(stderr, "usage: %s <server-address> <packet-size> \n", argv[0]);
    return 1;
  }

  rc_init(
    on_pre_conn,
    NULL, // on connect
    on_completion,
    NULL); // on disconnect
  rc_client_loop(argv[1], DEFAULT_PORT, &ctx);
  return 0;
}