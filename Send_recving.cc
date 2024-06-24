#include <stdio.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include <sched.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <stdexcept>
#include <assert.h>

#include <hiredis/hiredis.h>
#include <hiredis/adapters/poll.h>
#include <hiredis/async.h>

#define DIVUP(x, y) (((x)+(y)-1)/(y))
#define ROUNDUP(x, y) (DIVUP((x), (y))*(y))
static inline void ib_malloc(void** ptr, size_t size) {
  size_t page_size = sysconf(_SC_PAGESIZE);
  void* p;
  int size_aligned = ROUNDUP(size, page_size);
  int ret = posix_memalign(&p, page_size, size_aligned);
  if (ret != 0) {
    printf("posix_memalign error.\n");
    exit(1);
  }
  memset(p, 0, size);
  *ptr = p;
}
int init_enable  = 0;
int trace_enable = 0;
int debug_enable = 0;
#define LOG_INIT    if (init_enable ) printf
#define LOG_TRACE   if (trace_enable) printf
#define LOG_DEBUG   if (debug_enable) printf


struct DMAcontext {
    struct ibv_pd* pd;
    struct ibv_context* ctx;
    struct ibv_cq* receive_cq;
    struct ibv_cq* send_cq;
    struct ibv_mr* send_mr;
    void* send_region;
    struct ibv_qp* data_qp;
    struct ibv_qp* receive_qp;
    struct ibv_cq_ex* receive_recv_cq;
    struct ibv_mr* receive_mp_mr;
    struct ibv_sge* receive_sge;
    uint8_t* mp_recv_ring;
    struct ibv_wq *receive_wq;
    struct ibv_recv_wr *recv_wr;
};
#define PS_FILTER_TEMPLATE 0x05, 0x04, 0x03, 0x02, 0x01, 0xFF
#define WORKER_FILTER_TEMPLATE 0x77, 0x77, 0x77, 0x77, 0x77, 0xFF

// #define SRC_MAC 0xb8, 0x59, 0x9f, 0x1d, 0x04, 0xf2 
#define SRC_MAC 0xe4, 0x1d, 0x2d, 0xf3, 0xdd, 0xcc
#define DST_MAC 0x77, 0x77, 0x77, 0x77, 0x77, 0xFF

#define ETH_TYPE 0x07, 0x00

#define IP_HDRS 0x45, 0x00, 0x00, 0x54, 0x00, 0x00, 0x40, 0x00, 0x40, 0x01, 0xaf, 0xb6

#define SRC_IP 0x0d, 0x07, 0x38, 0x66

#define DST_IP 0x0d, 0x07, 0x38, 0x7f

#define SRC_PORT 0x67, 0x67

#define DST_PORT 0x78, 0x78

#define UDP_HDRS 0x00, 0x00, 0x00, 0x00

// Only a template, DST_IP will be modified soon
// This one is for sending
const unsigned char PS_IP_ETH_UDP_HEADER[] = { WORKER_FILTER_TEMPLATE, SRC_MAC, ETH_TYPE, IP_HDRS, SRC_IP, DST_IP };
// const unsigned char WORKER_IP_ETH_UDP_HEADER[] = { PS_FILTER_TEMPLATE, SRC_MAC, ETH_TYPE, IP_HDRS, SRC_IP, DST_IP };
const unsigned char WORKER_IP_ETH_UDP_HEADER[] = { DST_MAC, SRC_MAC, ETH_TYPE, IP_HDRS, SRC_IP, DST_IP };

#pragma pack(push, 1)
    struct agghdr {
        uint16_t bitmap;
        uint16_t num_worker;
        uint16_t flag_1;
        uint16_t flag_2;
        uint16_t flag_3;
        // reserved       :  2;
        // isForceFoward  :  1;

        // uint16_t appID;
        // uint16_t seq_num;
        // uint16_t agtr;
        // uint16_t agtr2;
};
#pragma pack(pop)

#define LAYER_SIZE 10 
#define IP_ETH_UDP_HEADER_SIZE 44 // 6 * 2 * 2 = 24 ETH + 20 IP + 10  = 54 16
#define ALL_PACKET_SIZE 70 // 44 -> 64
static constexpr size_t kAppMaxPostlist = 512;
static constexpr size_t kAppRingSize = 512;

void install_flow_rule()
{

}

void p4ml_header_print_h(agghdr *p4ml_header)
{
    // std::lock_guard<std::mutex> lock(_packet_print_mutex);
    printf("bitmap: %u, num_worker: %u "
           "agtr: %u agtr: %u, agtr2: %u",
        // caption,
        ntohs(p4ml_header->bitmap), p4ml_header->num_worker, 
        ntohs(p4ml_header->flag_1), ntohs(p4ml_header->flag_2), ntohs(p4ml_header->flag_3));

    printf("\n");
}

// int resolved(strct rdma_cm_id *id)
// {
//     struct 
// }
int my_send_queue_length = 2048;
int my_recv_queue_length = my_send_queue_length * 8;

unsigned char PS_FILTER_TEMPLATE_R[] = { 0x05, 0x04, 0x03, 0x02, 0x01, 0xFF };
unsigned char WORKER_FILTER_TEMPLATE_R[] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0xFF };

DMAcontext* DMA_create(ibv_device* ib_dev, int thread_id, bool isPS)
{
    ibv_context *context = ibv_open_device(ib_dev);
    if (!context) {
        fprintf(stderr, "Couldn't get context for %s\n",
            ibv_get_device_name(ib_dev));
        exit(1);
    }
    ibv_pd* pd = ibv_alloc_pd(context);
    if (!pd) {
        fprintf(stderr, "Couldn't allocate PD\n");
        exit(1);
    }
    struct ibv_cq* rec_cq = ibv_create_cq(context, my_recv_queue_length + 1, NULL, NULL, 0);
        if (!rec_cq) {
        fprintf(stderr, "Couldn't create CQ %d\n", errno);
        exit(1);
    }

    struct ibv_cq* snd_cq = ibv_create_cq(context, my_send_queue_length + 1, NULL, NULL, 0);
    if (!snd_cq) {
        fprintf(stderr, "Couldn't create CQ %d\n", errno);
        exit(1);
    }

    struct ibv_qp *qp;
    struct ibv_qp_init_attr_ex* qp_init_attr = 
            (struct ibv_qp_init_attr_ex*)malloc(sizeof(struct ibv_qp_init_attr_ex));
    
    memset(qp_init_attr, 0, sizeof(*qp_init_attr));

    qp_init_attr->qp_type = IBV_QPT_RAW_PACKET;
    qp_init_attr->send_cq = snd_cq;
    qp_init_attr->recv_cq = rec_cq;

    qp_init_attr->cap.max_send_wr = my_send_queue_length + 1;
    qp_init_attr->cap.max_send_sge = 1;
    qp_init_attr->cap.max_recv_wr = my_recv_queue_length + 1;
    qp_init_attr->cap.max_recv_sge = 1;
    qp_init_attr->cap.max_inline_data = 512;
    qp_init_attr->pd = pd;
    qp_init_attr->sq_sig_all = 0;
    // qp_init_attr->comp_mask = IBV_QP_INIT_ATTR_PD;
    
    qp_init_attr->comp_mask =
                            IBV_QP_INIT_ATTR_MAX_TSO_HEADER | IBV_QP_INIT_ATTR_PD;
    qp_init_attr->max_tso_header = IP_ETH_UDP_HEADER_SIZE;
    

    qp = ibv_create_qp_ex(context, qp_init_attr);
    if (!qp) {
        
        fprintf(stderr, "Couldn't create RSS QP\n");
        if (qp) {
            LOG_INIT("ibv_destroy_qp(%p)\n", qp);
            int ret = ibv_destroy_qp(qp);
            if (ret) {
                fprintf(stderr, "ibv_destroy_qp() failed, ret %d, errno %d: %s\n", ret, errno, strerror(errno));
            }
        }
        exit(1);
    }

    struct ibv_qp_attr qp_attr;
    int qp_flags;
    int ret;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_flags = IBV_QP_STATE | IBV_QP_PORT;
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.port_num = 1;
    ret = ibv_modify_qp(qp, &qp_attr, qp_flags);
    if (ret < 0) {
        fprintf(stderr, "failed modify qp to init\n");

        if (qp) {
            LOG_INIT("ibv_destroy_qp(%p)\n", qp);
            ret = ibv_destroy_qp(qp);
            if (ret) {
                fprintf(stderr, "ibv_destroy_qp() failed, ret %d, errno %d: %s\n", ret, errno, strerror(errno));
            }
        }
        exit(1);
        // exit(1);
        // exit(1);
    }
    memset(&qp_attr, 0, sizeof(qp_attr));

    //  move the qp to receive
    qp_flags = IBV_QP_STATE;
    qp_attr.qp_state = IBV_QPS_RTR;
    ret = ibv_modify_qp(qp, &qp_attr, qp_flags);
    if (ret < 0) {
        fprintf(stderr, "failed modify qp to receive\n");
        if (ret < 0) {
        fprintf(stderr, "failed modify qp to send\n");
        if (qp) {
            LOG_INIT("ibv_destroy_qp(%p)\n", qp);
            ret = ibv_destroy_qp(qp);
            if (ret) {
                fprintf(stderr, "ibv_destroy_qp() failed, ret %d, errno %d: %s\n", ret, errno, strerror(errno));
            }
        }
        exit(1);
        // exit(1);
    };
        // exit(1);
    }
    //  move the qp to send
    qp_flags = IBV_QP_STATE;
    qp_attr.qp_state = IBV_QPS_RTS;
    ret = ibv_modify_qp(qp, &qp_attr, qp_flags);
    if (ret < 0) {
        fprintf(stderr, "failed modify qp to send\n");
        if (qp) {
            LOG_INIT("ibv_destroy_qp(%p)\n", qp);
            ret = ibv_destroy_qp(qp);
            if (ret) {
                fprintf(stderr, "ibv_destroy_qp() failed, ret %d, errno %d: %s\n", ret, errno, strerror(errno));
            }
        }
        exit(1);
        // exit(1);
    }

    int send_buf_size = (LAYER_SIZE + IP_ETH_UDP_HEADER_SIZE) * my_send_queue_length + 1;

    void *send_buf;
    ib_malloc(&send_buf, send_buf_size);
    if (!send_buf) {
        fprintf(stderr, "Coudln't allocate send memory\n");
        exit(1);
    }

    struct ibv_mr* send_mr;
    send_mr = ibv_reg_mr(pd, send_buf, send_buf_size, IBV_ACCESS_LOCAL_WRITE);
    if (!send_mr) {
        fprintf(stderr, "Couldn't register recv mr\n");
        if (send_buf) {
            LOG_INIT("Free memory buffer (%p)\n", send_buf);
            free(send_buf);
        }
        exit(1);
    }
    /* Recv Queue*/
    struct ibv_cq_init_attr_ex cq_init_attr;
    memset(&cq_init_attr, 0, sizeof(cq_init_attr));
    cq_init_attr.comp_mask = IBV_CQ_INIT_ATTR_MASK_FLAGS;
    cq_init_attr.flags = IBV_CREATE_CQ_ATTR_IGNORE_OVERRUN;
    cq_init_attr.cqe = 512;

    struct ibv_cq_ex *mp_recv_cq = ibv_create_cq_ex(context, &cq_init_attr);
    assert(mp_recv_cq != nullptr);


    struct ibv_wq_init_attr wq_init_attr = {};
    wq_init_attr.wq_type = IBV_WQT_RQ;
    wq_init_attr.max_wr = 512;
    wq_init_attr.max_sge = 1;
    wq_init_attr.pd = pd;
    // wq_init_attr.cq = mp_recv_cq;
    wq_init_attr.cq = ibv_cq_ex_to_cq(mp_recv_cq);
    
    struct ibv_wq *mp_wq = ibv_create_wq(context, &wq_init_attr);
    assert(mp_wq != nullptr);

    struct ibv_wq_attr wq_attr;
    memset(&wq_attr, 0, sizeof(wq_attr));
    wq_attr.attr_mask = IBV_WQ_ATTR_STATE;
    wq_attr.wq_state = IBV_WQS_RDY;
    assert(ibv_modify_wq(mp_wq, &wq_attr) == 0);

    ibv_rwq_ind_table_init_attr mp_ind_table = {0};
    mp_ind_table.log_ind_tbl_size = 0;
    mp_ind_table.ind_tbl = &mp_wq;
    mp_ind_table.comp_mask = 0;
    struct ibv_rwq_ind_table *mp_ind_tbl = ibv_create_rwq_ind_table(context, &mp_ind_table);
    assert(mp_ind_tbl != nullptr);

    // Create rx_hash_conf and indirection table for the QP
    uint8_t toeplitz_key[] = { 0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
        0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
        0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
        0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
        0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa };
    const int TOEPLITZ_RX_HASH_KEY_LEN = sizeof(toeplitz_key) / sizeof(toeplitz_key[0]);

    struct ibv_rx_hash_conf rss_conf = {
        .rx_hash_function = IBV_RX_HASH_FUNC_TOEPLITZ,
        .rx_hash_key_len = TOEPLITZ_RX_HASH_KEY_LEN,
        .rx_hash_key = toeplitz_key,
        .rx_hash_fields_mask = IBV_RX_HASH_DST_PORT_UDP,
    };

    struct ibv_qp_init_attr_ex mp_qp_init_attr;
    memset(&mp_qp_init_attr, 0, sizeof(mp_qp_init_attr));
    mp_qp_init_attr.pd = pd;
    mp_qp_init_attr.qp_type = IBV_QPT_RAW_PACKET;
    mp_qp_init_attr.comp_mask = IBV_QP_INIT_ATTR_PD | IBV_QP_INIT_ATTR_IND_TABLE | IBV_QP_INIT_ATTR_RX_HASH |IBV_QP_INIT_ATTR_CREATE_FLAGS;
    
    mp_qp_init_attr.rx_hash_conf = rss_conf;
    mp_qp_init_attr.rwq_ind_tbl = mp_ind_tbl;

    struct ibv_qp* mp_recv_qp = ibv_create_qp_ex(context, &mp_qp_init_attr);
    if (!mp_recv_qp)
	{   
        fprintf(stderr, "1111111111Q %d\n", errno);
        exit(1);
    }
    assert(mp_recv_qp != nullptr);
    
    size_t tx_ring_size = LAYER_SIZE * kAppMaxPostlist;
    uint8_t* mp_send_ring;
    ib_malloc((void **)&mp_send_ring, tx_ring_size + 1);
    assert(mp_send_ring != nullptr);
    memset(mp_send_ring, 0, tx_ring_size + 1);

    struct ibv_mr* mp_send_mr = ibv_reg_mr(pd, mp_send_ring, tx_ring_size, IBV_ACCESS_LOCAL_WRITE);
    assert(mp_send_mr != nullptr);

    // Register RX ring memory
    uint8_t* mp_recv_ring;
    size_t rx_ring_size = ALL_PACKET_SIZE * kAppRingSize;
    ib_malloc((void **)&mp_recv_ring, rx_ring_size + 1);
    assert(mp_recv_ring != nullptr);
    memset(mp_recv_ring, 0, rx_ring_size + 1);

    struct ibv_mr* mp_mr = ibv_reg_mr(pd, mp_recv_ring, rx_ring_size, IBV_ACCESS_LOCAL_WRITE);
    assert(mp_mr != nullptr);

    struct raw_eth_flow_attr {
            struct ibv_flow_attr            attr;
            struct ibv_flow_spec_eth        spec_eth;
            // struct ibv_flow_spec_ipv4       spec_ipv4;
    } __attribute__((packed));
    struct raw_eth_flow_attr flow_attr = {
        .attr = {
                .comp_mask      = 0,
                .type           = IBV_FLOW_ATTR_NORMAL,
                .size           = sizeof(flow_attr),
                .priority       = 0,
                .num_of_specs   = 1,
                .port           = 1,
                .flags          = 0,
        },
        .spec_eth = {
                .type   = IBV_FLOW_SPEC_ETH,
                .size   = sizeof(struct ibv_flow_spec_eth),
                .val = {
                        // .dst_mac = {0x77, 0x77, 0x77, 0x77, 0x77, 0xFF},
                        .dst_mac = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                        .src_mac = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                        .ether_type = 0,
                        .vlan_tag = 0,
                },
                .mask = {
                        // .dst_mac = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
                        .dst_mac = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                        .src_mac = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
                        .ether_type = 0,
                        .vlan_tag = 0,
                }
        }
    };
    
    struct ibv_flow *eth_flow;
    eth_flow = ibv_create_flow(mp_recv_qp, &(flow_attr.attr));
    if (!eth_flow)
	{   
        fprintf(stderr, "ibv_create_flow %d\n", errno);
        exit(1);
    }

    struct ibv_qp_attr rx_qp_attr;
    memset(&rx_qp_attr, 0, sizeof(&rx_qp_attr));
    rx_qp_attr.qp_state = IBV_QPS_INIT;
    rx_qp_attr.port_num = 1;
    int err = ibv_modify_qp(mp_recv_qp, 
                            &rx_qp_attr, 
                            IBV_QP_STATE | IBV_QP_PORT);

    memset(&rx_qp_attr, 0, sizeof(&rx_qp_attr));
    rx_qp_attr.qp_state = IBV_QPS_RTR;
    err = ibv_modify_qp(mp_recv_qp, 
                            &rx_qp_attr, 
                            IBV_QP_STATE);
    struct ibv_sge* mp_sge = reinterpret_cast<struct ibv_sge*>(malloc(sizeof(struct ibv_sge) * kAppRingSize));
    struct ibv_recv_wr* recv_wr = reinterpret_cast<struct ibv_recv_wr*>(malloc(sizeof(struct ibv_recv_wr) * kAppRingSize));
    for (int i = 0; i < kAppRingSize; i++)
    {
        size_t mpwqe_offset = i * (ALL_PACKET_SIZE);
        mp_sge[i].addr = reinterpret_cast<uint64_t>(&mp_recv_ring[mpwqe_offset]);
        // printf("mp_sge %llu\n", mp_sge[i].addr);
        mp_sge[i].length = ALL_PACKET_SIZE;
        mp_sge[i].lkey = mp_mr->lkey;
        recv_wr[i].wr_id = i;
        recv_wr[i].sg_list = &(mp_sge[i]);
        recv_wr[i].num_sge = 1;
    }
    return new DMAcontext{
        .pd = pd,
        .ctx = context,
        .receive_cq = rec_cq,
        .send_cq = snd_cq,
        .send_mr = send_mr,
        .send_region = send_buf,
        .data_qp = qp,
        .receive_qp = mp_recv_qp,
        .receive_recv_cq = mp_recv_cq,
        .receive_mp_mr = mp_mr,
        .receive_sge = mp_sge,
        .mp_recv_ring = mp_recv_ring,
        .receive_wq = mp_wq,
        .recv_wr = recv_wr,
    };
}

static inline int recv(DMAcontext *dma)
{
    // struct ibv_recv_wr wr;
    struct ibv_recv_wr *bad_wr;
	// struct ibv_sge sge;
    int ret = 0;

    // first num_post_recv  = kAppRingSize
    for (int i=0; i<kAppRingSize; i++) {
        printf("post %d\n", i);
        ret = ibv_post_wq_recv(dma->receive_wq, &dma->recv_wr[i], &bad_wr);
        if (ret != 0)
        {
            printf("ret %d\n", ret);
        }
    }

    struct ibv_poll_cq_attr attr = {};
    struct ibv_wc wc = {};
    struct ibv_wc_tm_info tm_info = {};
    bool end_flag = false;
    // struct agghdr* agg_header;
    int recvnum = kAppRingSize * 2;
    do {
        ret = ibv_start_poll(dma->receive_recv_cq, &attr);
        printf("ret : %d\n", ret);
        sleep(1);
        if (ret != ENOENT)
        {
            recvnum--;
            wc.opcode = ibv_wc_read_opcode(dma->receive_recv_cq);
            wc.wc_flags = ibv_wc_read_wc_flags(dma->receive_recv_cq);
            uint64_t index = (uintptr_t)dma->receive_recv_cq->wr_id;
            // wc->wr_id
            printf("wr_id :%d\n", index);
            if (dma->receive_recv_cq->status != IBV_WC_SUCCESS) {
                fprintf(stderr, "ibv_start_poll, Failed status \"%s\" (%d) for wr_id %d\n",
                ibv_wc_status_str(dma->receive_recv_cq->status), dma->receive_recv_cq->status, (int) dma->receive_recv_cq->wr_id);
            }
            
            
            // comsume recv buf
            uint8_t* buf = &dma->mp_recv_ring[index * ALL_PACKET_SIZE];
            agghdr* p4ml_header = reinterpret_cast<agghdr*>(buf + IP_ETH_UDP_HEADER_SIZE);
            p4ml_header_print_h(p4ml_header);

            // post_new_recv of wr_id = index
            ret = ibv_post_wq_recv(dma->receive_wq, &dma->recv_wr[index], &bad_wr);
            if (ret != 0)
            {
                printf("continue post recv first ret %d\n", ret);
            }
        }
    } while (ret == ENOENT);

    do {
        ret = ibv_next_poll(dma->receive_recv_cq);
        wc.opcode = ibv_wc_read_opcode(dma->receive_recv_cq);
        wc.wc_flags = ibv_wc_read_wc_flags(dma->receive_recv_cq);
        uint64_t index = (uintptr_t)dma->receive_recv_cq->wr_id;
        // wc->wr_id
        printf("continue poll cq wr_id :%d\n", index);
        if (dma->receive_recv_cq->status != IBV_WC_SUCCESS) {
            fprintf(stderr, "ibv_start_poll, Failed status \"%s\" (%d) for wr_id %d\n",
            ibv_wc_status_str(dma->receive_recv_cq->status), dma->receive_recv_cq->status, (int) dma->receive_recv_cq->wr_id);
        }
        
        // comsume the next recv buf
        uint8_t* buf = &dma->mp_recv_ring[index * ALL_PACKET_SIZE];
        agghdr* p4ml_header = reinterpret_cast<agghdr*>(buf + IP_ETH_UDP_HEADER_SIZE);
        p4ml_header_print_h(p4ml_header);

        // post_new_recv of wr_id = index
        ret = ibv_post_wq_recv(dma->receive_wq, &dma->recv_wr[index], &bad_wr);
        if (ret != 0)
        {
            printf("continue post recv first ret %d\n", ret);
        }
        recvnum--;
    } while (ret == ENOENT || recvnum > 0);


    ibv_end_poll(dma->receive_recv_cq);
    

    return 1;

}

void send_packet(DMAcontext *dma_context, int chunk_size, int offset)
{
    int ret;
    struct ibv_sge sg;
    struct ibv_send_wr wr, *bad_wr;
    
    memset(&sg, 0, sizeof(sg));
    
    sg.addr = (uintptr_t)((char *)dma_context->send_region + offset * (LAYER_SIZE));
    // printf("%d\n", sg.addr);

    sg.length = chunk_size;
    sg.lkey = dma_context->send_mr->lkey;

    
    char hdr[IP_ETH_UDP_HEADER_SIZE];
    memcpy(hdr, WORKER_IP_ETH_UDP_HEADER, IP_ETH_UDP_HEADER_SIZE);
    
    memset(&wr, 0, sizeof(wr));
    
    wr.wr_id = 0;
    wr.sg_list = &sg;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_TSO;
    wr.tso.mss = LAYER_SIZE;
    wr.tso.hdr_sz = IP_ETH_UDP_HEADER_SIZE;
    

    wr.send_flags |= IBV_SEND_SIGNALED;
    wr.tso.hdr = hdr;
    
    // printf("hdr :%s", hdr);

    ret = ibv_post_send(dma_context->data_qp, &wr, &bad_wr);
    // printf("bad_wr.wr_id :%d", bad_wr->wr_id);
    if (ret < 0)
    {
        fprintf(stderr, "failed to post send\n");
        exit(1);
    }

    struct ibv_wc wc_send_cq[1024];
    ibv_poll_cq(dma_context->send_cq, 1024, wc_send_cq);

}

 int main(int argc, char **argv)
 {
    struct addrinfo *addr;
    struct rdma_cm_event *event =NULL;
    struct rdma_cm_id *conn = NULL;
    struct rdma_event_channel *ec = NULL;
    struct DMAcontext* dma = NULL;

    struct ibv_device** dev_list;
    struct ibv_device* ib_dev;
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list)
    {
        perror("Failed to get devices list");
        exit(1);
    }
    ib_dev = dev_list[0];
    if (!ib_dev)
    {
        fprintf(stderr, "IB device not found\n");
        exit(1);
    }
    int num_thread = 1;
    for (int i = 0; i < num_thread; i++)
    {
        dma = DMA_create(ib_dev, i, 0);
    }
    // int num = recv(dma);
    printf("using: %s\n", ibv_get_device_name(ib_dev));
    
    int packet_num = 1024;
    for (int j = 0; j < 8; j++)
    {
        for (int i = 0; i < packet_num; i++)
        {
            i = i % my_send_queue_length;
            agghdr *agg_header = (agghdr*)(dma->send_region + i * (LAYER_SIZE));
            agg_header->bitmap = 11111;
            agg_header->num_worker = 1;
            agg_header->flag_1 = 1;
            agg_header->flag_2 = 1;
            agg_header->flag_3 = i;
        }
        send_packet(dma, (LAYER_SIZE) * packet_num, 0);
    }
    
 }
