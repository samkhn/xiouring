#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 8000
#define BACKLOG_SIZE 512

#define MAX_CONNS 4096
#define BUFFER_COUNT MAX_CONNS
#define MAX_MSG_LEN 2048

static char buffers[BUFFER_COUNT][MAX_MSG_LEN] = { 0 };

enum {
	ACCEPT,
	READ,
	WRITE,
	PROV_BUF,
};

typedef struct conn_info {
	__u32 fd;
	__u16 type;
	__u16 bid;
} conn_info;

static void sigint_handler(int signum)
{
	printf("SIGINT received. Shutting down...\n");
	exit(0);
}

int main()
{
	// Socket setup
	struct sockaddr_in server_addr;
	int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	int enable = 1;
	if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &enable,
		       sizeof(enable)) < 0) {
		perror("setsockopt failed");
		exit(1);
	}
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(DEFAULT_PORT);
	server_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(listen_sock, (struct sockaddr *)&server_addr,
		 sizeof(server_addr)) < 0) {
		perror("bind failed");
		exit(1);
	}
	if (listen(listen_sock, BACKLOG_SIZE) < 0) {
		perror("listen failed");
		exit(1);
	}
	printf("iouring cat server listening on port %d\n", DEFAULT_PORT);

	// iouring setup
	struct io_uring ring;
	struct io_uring_params params;
	memset(&params, 0, sizeof(params));
	if (io_uring_queue_init_params(64, &ring, &params) < 0) {
		perror("io_uring_queue_init_params failed");
		exit(1);
	}
	if (!(params.features & IORING_FEAT_FAST_POLL)) {
		perror("kernel fast poll unavailable");
		exit(1);
	}

	struct io_uring_probe *probe = io_uring_get_probe_ring(&ring);
	if (!probe) {
		perror("alloc probe failed");
		exit(1);
	}
	if (!io_uring_opcode_supported(probe, IORING_OP_PROVIDE_BUFFERS)) {
		perror("io_uring PROVIDE BUFFER unavailable");
		exit(1);
	}

	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_provide_buffers(sqe, buffers, MAX_MSG_LEN, BUFFER_COUNT,
				      0, 0);

	if (io_uring_submit(&ring) < 0) {
		perror("io_uring_submit failed");
		exit(1);
	}
	io_uring_wait_cqe(&ring, &cqe);
	if (cqe->res < 0) {
		perror("io_uring_wait_cqe failed");
		exit(1);
	}
	io_uring_cqe_seen(&ring, cqe);

	struct sockaddr_in client_addr;
	socklen_t client_len = sizeof(client_addr);

	sqe = io_uring_get_sqe(&ring);
	io_uring_prep_accept(sqe, listen_sock, (struct sockaddr *)&client_addr,
			     &client_len, 0);
	io_uring_sqe_set_flags(sqe, 0);
	conn_info i = { .fd = listen_sock, .type = READ };
	memcpy(&sqe->user_data, &i, sizeof(i));

	// listening
	signal(SIGINT, sigint_handler);
	while (1) {
		io_uring_submit_and_wait(&ring, 1);
		struct io_uring_cqe *cqe;
		uint32_t head;
		uint32_t count = 0;
		io_uring_for_each_cqe(&ring, head, cqe)
		{
			count++;
			struct conn_info i;
			memcpy(&i, &cqe->user_data, sizeof(i));
			if (cqe->res == -ENOBUFS) {
				perror("cqe read res ENOBUFS");
				exit(1);
			} else if (i.type == PROV_BUF) {
				if (cqe->res < 0) {
					perror("PROV_BUF cqe->res error");
					exit(1);
				}
			} else if (i.type == ACCEPT) {
				int sock_connection_fd = cqe->res;
				if (sock_connection_fd >= 0) {
					struct io_uring_sqe *read_sqe =
						io_uring_get_sqe(&ring);
					io_uring_prep_recv(read_sqe,
							   listen_sock, NULL,
							   MAX_MSG_LEN, 0);
					io_uring_sqe_set_flags(
						read_sqe, IOSQE_BUFFER_SELECT);
					read_sqe->buf_group = 0;
					i.type = READ;
					memcpy(&read_sqe->user_data, &i,
					       sizeof(i));
				}
				struct io_uring_sqe *reaccept_sqe =
					io_uring_get_sqe(&ring);
				io_uring_prep_accept(
					reaccept_sqe, listen_sock,
					(struct sockaddr *)&client_addr,
					&client_len, 0);
				io_uring_sqe_set_flags(reaccept_sqe, 0);
				i.type = ACCEPT;
				memcpy(&reaccept_sqe, &i, sizeof(i));
			} else if (i.type == READ) {
				int bytes_read = cqe->res;
				int buffer_id = cqe->flags >> 16;
				if (bytes_read <= 0) {
					struct io_uring_sqe *provide_sqe =
						io_uring_get_sqe(&ring);
					io_uring_prep_provide_buffers(
						provide_sqe, buffers[buffer_id],
						MAX_MSG_LEN, 1, 0, buffer_id);
					i.fd = 0;
					i.type = PROV_BUF;
					memcpy(&provide_sqe, &i, sizeof(i));
					close(i.fd);
				} else {
					struct io_uring_sqe *write_sqe =
						io_uring_get_sqe(&ring);
					io_uring_prep_send(write_sqe,
							   listen_sock,
							   &buffers[buffer_id],
							   MAX_MSG_LEN, 0);
					io_uring_sqe_set_flags(write_sqe, 0);
					i.type = WRITE;
					i.bid = buffer_id;
					memcpy(&write_sqe->user_data, &i,
					       sizeof(i));
				}
			} else if (i.type == WRITE) {
				struct io_uring_sqe *provide_sqe =
					io_uring_get_sqe(&ring);
				io_uring_prep_provide_buffers(provide_sqe,
							      buffers[i.bid],
							      MAX_MSG_LEN, 1, 0,
							      i.bid);
				i.fd = 0;
				i.type = PROV_BUF;
				memcpy(&provide_sqe, &i, sizeof(i));
				struct io_uring_sqe *read_sqe =
					io_uring_get_sqe(&ring);
				io_uring_prep_recv(read_sqe, listen_sock, NULL,
						   MAX_MSG_LEN, 0);
				io_uring_sqe_set_flags(read_sqe,
						       IOSQE_BUFFER_SELECT);
				read_sqe->buf_group = 0;
				i.type = READ;
				memcpy(&read_sqe->user_data, &i, sizeof(i));
			} else {
				perror("Unknown message type received");
				exit(1);
			}
			io_uring_cq_advance(&ring, count);
		}
	}
	free(probe);
	exit(0);
}
