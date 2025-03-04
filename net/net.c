/*
   Copyright 2015 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#define NODELAY
#define NOLINGER
#ifdef __linux__
#define TCPBUFSZ
#endif

/*#define PTHREAD_USERFUNC*/

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <alloca.h>
#include <ctrace.h>

#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include "thread_util.h"

#ifdef __DGUX__
#include <siginfo.h>
#endif
#ifdef __sun
#include <siginfo.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#ifdef _AIX
#include <sys/socketvar.h>
#endif
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pwd.h>
#include <dirent.h>
#include <utime.h>
#include <sys/time.h>
#include <poll.h>

#include <bb_oscompat.h>
#include <compat.h>

#include <pool.h>
#include <plhash.h>
#include <assert.h>

#include "locks_wrap.h"
#include "net.h"
#include "net_int.h"

/* rtcpu.h breaks dbx on sun */
#ifndef NET_DEBUG
#include <rtcpu.h>
#endif

#include <endian_core.h>
#include <compile_time_assert.h>

#include <portmuxusr.h>

#include <epochlib.h>
#include <str0.h>

#include <util.h>
#include <sched.h>
#include <cdb2_constants.h>
#include "intern_strings.h"

#include <fsnapf.h>

#include "rtcpu.h"

#include "mem_net.h"
#include "mem_override.h"
#include <bdb_net.h>

#include "debug_switches.h"
#include "perf.h"

#include <crc32c.h>

#ifdef UDP_DEBUG
static int curr_udp_cnt = 0;
#endif

#define MILLION 1000000
#define BILLION 1000000000

extern int gbl_pmux_route_enabled;
extern int gbl_exit;
extern int gbl_net_portmux_register_interval;

int gbl_verbose_net = 0;
int subnet_blackout_timems = 5000;

#ifdef PER_THREAD_MALLOC
#define HOST_MALLOC(h, sz) comdb2_malloc((h)->msp, (sz))
#else
#define HOST_MALLOC(h, sz) malloc(sz)
#endif /* PER_THREAD_MALLOC */

void net_set_subnet_blackout(int ms)
{
    if (ms >= 0) {
        subnet_blackout_timems = ms;
    }
}

static unsigned long long gettmms(void)
{
    struct timeval tm;

    gettimeofday(&tm, NULL);

    return 1000 * ((unsigned long long)tm.tv_sec) +
           ((unsigned long long)tm.tv_usec) / 1000;
}

static int sbuf2read_wrapper(SBUF2 *sb, char *buf, int nbytes)
{
    if (debug_switch_verbose_sbuf())
        logmsg(LOGMSG_USER, "reading, reading %llu\n", gettmms());

    return sbuf2unbufferedread(sb, buf, nbytes);
}

static int sbuf2write_wrapper(SBUF2 *sb, const char *buf, int nbytes)
{
    if (debug_switch_verbose_sbuf())
        logmsg(LOGMSG_USER, "writing, writing %llu\n", gettmms());

    return sbuf2unbufferedwrite(sb, buf, nbytes);
}

extern void myfree(void *ptr);

/* Help me build the test program... - Sam J */
#ifdef TEST
static void myfree(void *ptr)
{
    if (ptr)
        free(ptr);
}
#endif

static sanc_node_type *add_to_sanctioned_nolock(netinfo_type *netinfo_ptr,
                                                const char hostname[],
                                                int portnum);
static int process_hello_common(netinfo_type *netinfo_ptr,
                                host_node_type *host_node_ptr,
                                int look_for_magic);
static int process_hello(netinfo_type *netinfo_ptr,
                         host_node_type *host_node_ptr);
static int process_hello_reply(netinfo_type *netinfo_ptr,
                               host_node_type *host_node_ptr);
/* '_ll' lockless -- the caller should be holding the netinfo_ptr->lock */
static host_node_type *get_host_node_by_name_ll(netinfo_type *netinfo_ptr,
                                                const char name[]);
static int connect_to_host(netinfo_type *netinfo_ptr,
                           host_node_type *host_node_ptr,
                           host_node_type *sponsor_host);
static int read_connect_message(SBUF2 *sb, char hostname[], int hostnamel,
                                int *portnum, netinfo_type *netinfo_ptr);
static void *accept_thread(void *arg);
static void *heartbeat_send_thread(void *arg);
static void *heartbeat_check_thread(void *arg);
static void *writer_thread(void *args);
static void *reader_thread(void *arg);
static void *connect_thread(void *arg);

static int net_writes(SBUF2 *sb, const char *buf, int nbytes);
static int net_reads(SBUF2 *sb, char *buf, int nbytes);

static watchlist_node_type *get_watchlist_node(SBUF2 *, const char *funcname);

int sbuf2ungetc(char c, SBUF2 *sb);

static int net_portmux_hello(void *);

/* We can't change the on-wire protocol easily.  So it
 * retains node numbers, but they're unused for now */
/* type 0 is internal connect message.
   type >0 is for applications */
typedef struct {
    char to_hostname[HOSTNAME_LEN];
    int to_portnum;
    int flags; /* was `int to_nodenum` */
    char my_hostname[HOSTNAME_LEN];
    int my_portnum;
    int my_nodenum;
} connect_message_type;

/* flags for connect_message_typs */
#define CONNECT_MSG_SSL 0x80000000
#define CONNECT_MSG_TONODE 0x0000ffff /* backwards compatible */

enum {
    NET_CONNECT_MESSAGE_TYPE_LEN = HOSTNAME_LEN + sizeof(int) + sizeof(int) +
                                   HOSTNAME_LEN + sizeof(int) + sizeof(int)
};

BB_COMPILE_TIME_ASSERT(net_connect_message_type,
                       sizeof(connect_message_type) ==
                           NET_CONNECT_MESSAGE_TYPE_LEN);

/* Endian manipulation routines */
static uint8_t *net_connect_message_put(const connect_message_type *msg_ptr,
                                        uint8_t *p_buf,
                                        const uint8_t *p_buf_end)
{
    int node = 0;
    if (p_buf_end < p_buf || NET_CONNECT_MESSAGE_TYPE_LEN > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_no_net_put(&(msg_ptr->to_hostname),
                           sizeof(msg_ptr->to_hostname), p_buf, p_buf_end);
    p_buf = buf_put(&(msg_ptr->to_portnum), sizeof(msg_ptr->to_portnum), p_buf,
                    p_buf_end);
    p_buf =
        buf_put(&(msg_ptr->flags), sizeof(msg_ptr->flags), p_buf, p_buf_end);
    p_buf = buf_no_net_put(&(msg_ptr->my_hostname),
                           sizeof(msg_ptr->my_hostname), p_buf, p_buf_end);
    p_buf = buf_put(&(msg_ptr->my_portnum), sizeof(msg_ptr->my_portnum), p_buf,
                    p_buf_end);
    p_buf = buf_put(&node, sizeof(msg_ptr->my_nodenum), p_buf, p_buf_end);

    return p_buf;
}

static const uint8_t *net_connect_message_get(connect_message_type *msg_ptr,
                                              const uint8_t *p_buf,
                                              const uint8_t *p_buf_end)
{
    int node = 0;
    if (p_buf_end < p_buf || NET_CONNECT_MESSAGE_TYPE_LEN > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_no_net_get(&(msg_ptr->to_hostname),
                           sizeof(msg_ptr->to_hostname), p_buf, p_buf_end);
    p_buf = buf_get(&(msg_ptr->to_portnum), sizeof(msg_ptr->to_portnum), p_buf,
                    p_buf_end);
    p_buf =
        buf_get(&(msg_ptr->flags), sizeof(msg_ptr->flags), p_buf, p_buf_end);
    p_buf = buf_no_net_get(&(msg_ptr->my_hostname),
                           sizeof(msg_ptr->my_hostname), p_buf, p_buf_end);
    p_buf = buf_get(&(msg_ptr->my_portnum), sizeof(msg_ptr->my_portnum), p_buf,
                    p_buf_end);
    p_buf = buf_get(&node, sizeof(msg_ptr->my_nodenum), p_buf, p_buf_end);

    return p_buf;
}

static uint8_t *net_wire_header_put(const wire_header_type *header_ptr,
                                    uint8_t *p_buf, const uint8_t *p_buf_end)
{
    int node = 0;
    if (p_buf_end < p_buf || NET_WIRE_HEADER_TYPE_LEN > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_no_net_put(&(header_ptr->fromhost),
                           sizeof(header_ptr->fromhost), p_buf, p_buf_end);
    p_buf = buf_put(&(header_ptr->fromport), sizeof(header_ptr->fromport),
                    p_buf, p_buf_end);
    p_buf = buf_put(&node, sizeof(header_ptr->fromnode), p_buf, p_buf_end);
    p_buf = buf_no_net_put(&(header_ptr->tohost), sizeof(header_ptr->tohost),
                           p_buf, p_buf_end);
    p_buf = buf_put(&(header_ptr->toport), sizeof(header_ptr->toport), p_buf,
                    p_buf_end);
    p_buf = buf_put(&node, sizeof(header_ptr->tonode), p_buf, p_buf_end);
    p_buf = buf_put(&(header_ptr->type), sizeof(header_ptr->type), p_buf,
                    p_buf_end);

    return p_buf;
}

static const uint8_t *net_wire_header_get(wire_header_type *header_ptr,
                                          const uint8_t *p_buf,
                                          const uint8_t *p_buf_end)
{
    int node = 0;
    if (p_buf_end < p_buf || NET_WIRE_HEADER_TYPE_LEN > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_no_net_get(&(header_ptr->fromhost),
                           sizeof(header_ptr->fromhost), p_buf, p_buf_end);
    p_buf = buf_get(&(header_ptr->fromport), sizeof(header_ptr->fromport),
                    p_buf, p_buf_end);
    p_buf = buf_get(&node, sizeof(header_ptr->fromnode), p_buf, p_buf_end);
    p_buf = buf_no_net_get(&(header_ptr->tohost), sizeof(header_ptr->tohost),
                           p_buf, p_buf_end);
    p_buf = buf_get(&(header_ptr->toport), sizeof(header_ptr->toport), p_buf,
                    p_buf_end);
    p_buf = buf_get(&node, sizeof(header_ptr->tonode), p_buf, p_buf_end);
    p_buf = buf_get(&(header_ptr->type), sizeof(header_ptr->type), p_buf,
                    p_buf_end);

    return p_buf;
}

typedef struct net_send_message_header {
    int usertype;
    int seqnum;
    int waitforack;
    int datalen;
} net_send_message_header;

enum { NET_SEND_MESSAGE_HEADER_LEN = 4 + 4 + 4 + 4 };

BB_COMPILE_TIME_ASSERT(net_send_message_header,
                       sizeof(net_send_message_header) ==
                           NET_SEND_MESSAGE_HEADER_LEN);

static const uint8_t *
net_send_message_header_put(const net_send_message_header *header_ptr,
                            uint8_t *p_buf, const uint8_t *p_buf_end)
{
    if (p_buf_end < p_buf || NET_SEND_MESSAGE_HEADER_LEN > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_put(&(header_ptr->usertype), sizeof(header_ptr->usertype),
                    p_buf, p_buf_end);
    p_buf = buf_put(&(header_ptr->seqnum), sizeof(header_ptr->seqnum), p_buf,
                    p_buf_end);
    p_buf = buf_put(&(header_ptr->waitforack), sizeof(header_ptr->waitforack),
                    p_buf, p_buf_end);
    p_buf = buf_put(&(header_ptr->datalen), sizeof(header_ptr->datalen), p_buf,
                    p_buf_end);

    return p_buf;
}

static const uint8_t *
net_send_message_header_get(net_send_message_header *header_ptr,
                            const uint8_t *p_buf, const uint8_t *p_buf_end)
{
    if (p_buf_end < p_buf || NET_SEND_MESSAGE_HEADER_LEN > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_get(&(header_ptr->usertype), sizeof(header_ptr->usertype),
                    p_buf, p_buf_end);
    p_buf = buf_get(&(header_ptr->seqnum), sizeof(header_ptr->seqnum), p_buf,
                    p_buf_end);
    p_buf = buf_get(&(header_ptr->waitforack), sizeof(header_ptr->waitforack),
                    p_buf, p_buf_end);
    p_buf = buf_get(&(header_ptr->datalen), sizeof(header_ptr->datalen), p_buf,
                    p_buf_end);

    return p_buf;
}

typedef struct net_ack_message_payload_type {
    int seqnum;
    int outrc;
    int paylen;
    char payload[4];
} net_ack_message_payload_type;

enum { NET_ACK_MESSAGE_PAYLOAD_TYPE_LEN = 4 + 4 + 4 + 4 };

BB_COMPILE_TIME_ASSERT(net_ack_message_payload_type,
                       sizeof(net_ack_message_payload_type) ==
                           NET_ACK_MESSAGE_PAYLOAD_TYPE_LEN);

static uint8_t *net_ack_message_payload_type_put(
    const net_ack_message_payload_type *payload_type_ptr, uint8_t *p_buf,
    const uint8_t *p_buf_end)
{
    if (p_buf_end < p_buf ||
        (offsetof(net_ack_message_payload_type, payload) +
         payload_type_ptr->paylen) > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_put(&(payload_type_ptr->seqnum),
                    sizeof(payload_type_ptr->seqnum), p_buf, p_buf_end);
    p_buf = buf_put(&(payload_type_ptr->outrc), sizeof(payload_type_ptr->outrc),
                    p_buf, p_buf_end);
    p_buf = buf_put(&(payload_type_ptr->paylen),
                    sizeof(payload_type_ptr->paylen), p_buf, p_buf_end);

    return p_buf;
}


static const uint8_t *
net_ack_message_payload_type_get(net_ack_message_payload_type *payload_type_ptr,
                                 const uint8_t *p_buf, const uint8_t *p_buf_end)
{
    if (p_buf_end < p_buf ||
        offsetof(net_ack_message_payload_type, payload) > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_get(&(payload_type_ptr->seqnum),
                    sizeof(payload_type_ptr->seqnum), p_buf, p_buf_end);
    p_buf = buf_get(&(payload_type_ptr->outrc), sizeof(payload_type_ptr->outrc),
                    p_buf, p_buf_end);
    p_buf = buf_get(&(payload_type_ptr->paylen),
                    sizeof(payload_type_ptr->paylen), p_buf, p_buf_end);

    return p_buf;
}


typedef struct net_ack_message_type {
    int seqnum;
    int outrc;
} net_ack_message_type;

enum { NET_ACK_MESSAGE_TYPE_LEN = 4 + 4 };

BB_COMPILE_TIME_ASSERT(net_ack_message_type, sizeof(net_ack_message_type) ==
                                                 NET_ACK_MESSAGE_TYPE_LEN);

static const uint8_t *
net_ack_message_type_put(const net_ack_message_type *p_net_ack_message_type,
                         uint8_t *p_buf, const uint8_t *p_buf_end)
{
    if (p_buf_end < p_buf || NET_ACK_MESSAGE_TYPE_LEN > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_put(&(p_net_ack_message_type->seqnum),
                    sizeof(p_net_ack_message_type->seqnum), p_buf, p_buf_end);
    p_buf = buf_put(&(p_net_ack_message_type->outrc),
                    sizeof(p_net_ack_message_type->outrc), p_buf, p_buf_end);

    return p_buf;
}

static const uint8_t *
net_ack_message_type_get(net_ack_message_type *p_net_ack_message_type,
                         const uint8_t *p_buf, const uint8_t *p_buf_end)
{
    if (p_buf_end < p_buf || NET_ACK_MESSAGE_TYPE_LEN > (p_buf_end - p_buf))
        return NULL;

    p_buf = buf_get(&(p_net_ack_message_type->seqnum),
                    sizeof(p_net_ack_message_type->seqnum), p_buf, p_buf_end);
    p_buf = buf_get(&(p_net_ack_message_type->outrc),
                    sizeof(p_net_ack_message_type->outrc), p_buf, p_buf_end);

    return p_buf;
}

typedef struct connect_and_accept {
    netinfo_type *netinfo_ptr;
    SBUF2 *sb;
    struct in_addr addr;
} connect_and_accept_t;

/* Close socket related to hostnode.  */
static void shutdown_hostnode_socket(host_node_type *host_node_ptr)
{
    if (gbl_verbose_net) {
        host_node_printf(LOGMSG_USER, host_node_ptr, "shutting down fd %d\n",
                         host_node_ptr->fd);
    }
    if (shutdown(host_node_ptr->fd, 2) != 0) {
        if (errno != ENOTCONN) {
            host_node_errf(LOGMSG_ERROR, host_node_ptr,
                           "%s: shutdown fd %d errno %d %s\n", __func__,
                           host_node_ptr->fd, errno, strerror(errno));
        }
    }
}

/* This must be called while holding the host_node_ptr->lock.
 *
 * This will call shutdown() on the fd, which will cause the reader & writer
 * threads (if any) to error out of any blocking io and exit.
 *
 * If there are no reader or writer threads left then this will properly
 * close the socket and sbuf.
 */
static void close_hostnode_ll(host_node_type *host_node_ptr)
{
    if (!host_node_ptr->closed) {
        host_node_ptr->closed = 1;

        /* this has to be done before notifying the sql transactions
           that connection dropped, otherwise they will race and
           send stuff before the host is reconnected; transaction is
           send to garbcan without notifying sql, or master, and they
           will never be applied */
        host_node_ptr->got_hello = 0;

        shutdown_hostnode_socket(host_node_ptr);

        /* wake up the writer thread if it's asleep */
        Pthread_cond_signal(&(host_node_ptr->write_wakeup));

        /* call the hostdown routine if provided */
        if (host_node_ptr->netinfo_ptr->hostdown_rtn) {
            (host_node_ptr->netinfo_ptr->hostdown_rtn)(
                host_node_ptr->netinfo_ptr, host_node_ptr->host);
            host_node_printf(LOGMSG_DEBUG, host_node_ptr, "back from hostdown_rtn\n");
        }
    }

    /* If we have an fd or sbuf, and no reader or writer thread, then
     * close the socket properly */
    if (host_node_ptr->have_reader_thread == 0 &&
        host_node_ptr->have_writer_thread == 0 &&
        host_node_ptr->really_closed == 0
    ){
        SBUF2 *sb = host_node_ptr->sb;
        if (sb) {
#           if WITH_SSL
            if (sslio_has_ssl(sb))
                sslio_close(sb, 1);
#           endif
            sbuf2close(host_node_ptr->sb);
            host_node_ptr->sb = NULL;
            if (gbl_verbose_net)
                host_node_printf(LOGMSG_DEBUG, host_node_ptr, "closing sbuf\n");
        }
        if (host_node_ptr->fd >= 0) {
            if (gbl_verbose_net)
                host_node_printf(LOGMSG_DEBUG, host_node_ptr, "close fd %d\n",
                                 host_node_ptr->fd);
            if (close(host_node_ptr->fd) != 0)
                host_node_errf(LOGMSG_ERROR, host_node_ptr, "%s close fd %d errno %d %s\n",
                               __func__, host_node_ptr->fd, errno,
                               strerror(errno));
            host_node_ptr->fd = -1;
        }
        host_node_ptr->really_closed = 1;
    }
}

static void close_hostnode(host_node_type *host_node_ptr)
{
    Pthread_mutex_lock(&(host_node_ptr->lock));
    close_hostnode_ll(host_node_ptr);
    Pthread_mutex_unlock(&(host_node_ptr->lock));
}

#ifdef LIST_DEBUG
static void check_list_sizes_lk(host_node_type *host_node_ptr)
{
    write_data *list_ptr;
    list_ptr = host_node_ptr->write_head;
    while (list_ptr) {
        if (list_ptr->len > BILLION)
            abort();
        list_ptr = list_ptr->next;
    }
}

static void check_list_sizes(host_node_type *host_node_ptr)
{
    Pthread_mutex_lock(&(host_node_ptr->enquelk));
    check_list_sizes_lk(host_node_ptr);
    Pthread_mutex_unlock(&(host_node_ptr->enquelk));
}
#endif

int gbl_print_net_queue_size = 0;

/* Enque a net message consisting of a header and some optional data.
 * The caller should hold the enque lock.
 * Note that dataptr1==NULL => datasz1==0 and dataptr2==NULL => datasz2==0
 */
static int write_list(netinfo_type *netinfo_ptr, host_node_type *host_node_ptr,
                      const wire_header_type *headptr, const struct iovec *iov,
                      int iovcount, int flags)
{
    write_data *insert;
    int ii;
    size_t datasz;
    char *ptr;
    int rc;

    Pthread_mutex_lock(&(host_node_ptr->enquelk));


    /* let 1 message always slip in */
    if (host_node_ptr->enque_count) {
        if ((flags & WRITE_MSG_NOLIMIT) == 0 &&
            ((host_node_ptr->enque_count > netinfo_ptr->max_queue) ||
             (host_node_ptr->enque_bytes > netinfo_ptr->max_bytes))) {
            host_node_ptr->num_queue_full++;

            rc = -2;
            goto out;
        }
    }

    /* Although generic, this logic was really added to ensure that we
     * don't double enque heartbeat messages.  Not sure how much this really
     * happens in practice. */
    if ((flags & WRITE_MSG_NODUPE) != 0 && host_node_ptr->write_head) {
        const wire_header_type *newitem = headptr;
        wire_header_type *headitem = &host_node_ptr->write_head->payload.header;
        if (newitem->type == headitem->type) {
            /* Dedupe this item */
            host_node_ptr->dedupe_count++;
            rc = 0;
            goto out;
        }
    }

    Pthread_mutex_unlock(&(host_node_ptr->enquelk));

    for (datasz = 0, ii = 0; ii < iovcount; ii++) {
        if (iov[ii].iov_base)
            datasz += iov[ii].iov_len;
    }
    if (netinfo_ptr->myhostname_len > HOSTNAME_LEN)
        datasz += netinfo_ptr->myhostname_len;
    if (host_node_ptr->hostname_len > HOSTNAME_LEN)
        datasz += host_node_ptr->hostname_len;

    time_metric_add(host_node_ptr->metric_queue_size, datasz);

    /* Malloc space for the list item struct (which includes the net message
     * header) and all the data in our iovec. */
    insert = HOST_MALLOC(host_node_ptr, sizeof(write_data) + datasz);
    if (insert == NULL) {
        logmsg(LOGMSG_ERROR, "%s: %s: %zu\n", __func__, strerror(errno),
               datasz);
        return -1;
    }

    insert->flags = flags;
    insert->enque_time = comdb2_time_epoch();
    insert->next = NULL;
    insert->prev = NULL;
    insert->len = sizeof(wire_header_type) + datasz;

    memcpy(&insert->payload.header, headptr, sizeof(wire_header_type));
    ptr = insert->payload.raw + sizeof(wire_header_type);
    // start = insert->payload.raw;

    /* if we have long hostnames, account for them here */
    if (netinfo_ptr->myhostname_len > HOSTNAME_LEN) {
        memcpy(ptr, netinfo_ptr->myhostname, netinfo_ptr->myhostname_len);
        ptr += netinfo_ptr->myhostname_len;
    }
    if (host_node_ptr->hostname_len > HOSTNAME_LEN) {
        memcpy(ptr, host_node_ptr->host, host_node_ptr->hostname_len);
        ptr += host_node_ptr->hostname_len;
    }
    for (ii = 0; ii < iovcount; ii++) {
        if (iov[ii].iov_base) {
            memcpy(ptr, iov[ii].iov_base, iov[ii].iov_len);
            ptr += iov[ii].iov_len;
        }
    }

    Pthread_mutex_lock(&(host_node_ptr->enquelk));

    if (host_node_ptr->write_head == NULL) {
        host_node_ptr->write_head = host_node_ptr->write_tail = insert;
        insert->next = insert->prev = NULL;
    } else if (flags & WRITE_MSG_HEAD) {
        /* Insert at head of list */
        insert->next = host_node_ptr->write_head;
        insert->prev = NULL;
        host_node_ptr->write_head->prev = insert;
        host_node_ptr->write_head = insert;
    } else if (flags & WRITE_MSG_INORDER && netinfo_ptr->netcmp_rtn != NULL) {
        int cnt = 0, cmp, reordered = 0;
        write_data *ptr = host_node_ptr->write_tail;

        while (ptr != NULL &&
               (cmp = (netinfo_ptr->netcmp_rtn)(
                    netinfo_ptr, insert->payload.raw, insert->len,
                    ptr->payload.raw, ptr->len)) < 0 &&
               cnt++ < netinfo_ptr->enque_reorder_lookahead) {
            reordered = 1;
            ptr = ptr->prev;
        }

        /* Update some stats */
        if (reordered) {
            netinfo_ptr->stats.reorders++;
            host_node_ptr->stats.reorders++;
        }

        /* Insert at head */
        if (ptr == NULL) {
            insert->next = host_node_ptr->write_head;
            insert->prev = NULL;
            host_node_ptr->write_head->prev = insert;
            host_node_ptr->write_head = insert;
        } else {
            insert->prev = ptr;
            insert->next = ptr->next;

            /* Normal case: will be at the tail */
            if (ptr == host_node_ptr->write_tail) {
                host_node_ptr->write_tail = insert;
            } else {
                ptr->next->prev = insert;
            }
            ptr->next = insert;
        }
    } else

    {
        /* Insert at tail of list */
        host_node_ptr->write_tail->next = insert;
        insert->prev = host_node_ptr->write_tail;
        insert->next = NULL;
        host_node_ptr->write_tail = insert;
    }

    if (netinfo_ptr->qstat_enque_rtn) {
        (netinfo_ptr->qstat_enque_rtn)(netinfo_ptr, host_node_ptr->qstat,
                                       insert->payload.raw, insert->len);
    }

    if (host_node_ptr->netinfo_ptr->trace && debug_switch_net_verbose())
        logmsg(LOGMSG_USER, "Queing %zu bytes %llu\n", insert->len, gettmms());
    host_node_ptr->enque_count++;
    if (host_node_ptr->enque_count > host_node_ptr->peak_enque_count) {
        host_node_ptr->peak_enque_count = host_node_ptr->enque_count;
        host_node_ptr->peak_enque_count_time = comdb2_time_epoch();
    }
    host_node_ptr->enque_bytes += insert->len;
    if (host_node_ptr->enque_bytes > host_node_ptr->peak_enque_bytes) {
        host_node_ptr->peak_enque_bytes = host_node_ptr->enque_bytes;
        host_node_ptr->peak_enque_bytes_time = comdb2_time_epoch();
    }

    rc = 0;

out:
    Pthread_mutex_unlock(&(host_node_ptr->enquelk));
    return rc;
}

static int read_stream(netinfo_type *netinfo_ptr, host_node_type *host_node_ptr,
                       SBUF2 *sb, void *inptr, int maxbytes)
{
    uint8_t *ptr = inptr;
    const int fd = sbuf2fileno(sb);
    int nread = 0;
    while (nread < maxbytes) {
        if (host_node_ptr) /* not set by all callers */
            host_node_ptr->timestamp = time(NULL);
        int n = sbuf2unbufferedread(sb, (char *)ptr + nread, maxbytes - nread);
        if (n > 0) {
            nread += n;
        } else if (n < 0) {
            if (errno == EAGAIN) { /* wait for some data */
                struct pollfd pol;
                pol.fd = fd;
                pol.events = POLLIN;
                if (poll(&pol, 1, -1) < 0) {
                    break;
                }
                if ((pol.revents & POLLIN) == 0) {
                    break;
                }
            } else if (errno == EINTR) { /* just read again */
                continue;
            } else {
                if (host_node_ptr)
                    host_node_printf(LOGMSG_USER, host_node_ptr, "%s:%s\n",
                                     __func__, strerror(errno));
                else
                    logmsgperror(__func__);
                break;
            }
        } else { /* n == 0; EOF */
            break;
        }
    }

    if (nread > 0) {
        if (netinfo_ptr) /* not set by all callers */
            netinfo_ptr->stats.bytes_read += nread;
        if (host_node_ptr)
            host_node_ptr->stats.bytes_read += nread;
    }

#if 0 
   printf("IN %s:%d %d\n", from, line, nread);
   fsnapf(stdout, inptr, nread);
#endif

    return nread;
}

/*
 * Retrieve the host_node_type by name.
 * Caller should be holding netinfo_ptr->lock.
 */
static host_node_type *get_host_node_by_name_ll(netinfo_type *netinfo_ptr,
                                                const char name[])
{
    if (!isinterned(name))
        abort();

    host_node_type *ptr = NULL;
    ptr = netinfo_ptr->head;
    while (ptr != NULL && ptr->host != name)
        ptr = ptr->next;

    return ptr;
}

static uint64_t net_delayed = 0;

// 10000 * 0.1 ms = 1s
#define net_delay_mult 10000

// don't delay > 5s
#define net_delay_max (5 * net_delay_mult)

void print_netdelay(void)
{
    int d = debug_switch_net_delay();
    double delay = (double)d / 10; // 0.1ms -> ms
    const char *status = "no";
    if (d && delay <= net_delay_max)
        status = "yes";
    logmsg(LOGMSG_USER, "netdelay=> delay:%.1fms delayed:%lu delaying:%s\n", delay,
           net_delayed, status);
}

static void timeval_to_timespec(struct timeval *tv, struct timespec *ts)
{
    ts->tv_sec = tv->tv_sec;
    ts->tv_nsec = tv->tv_usec * 1000;
}

static void timespec_to_timeval(struct timespec *ts, struct timeval *tv)
{
    tv->tv_sec = ts->tv_sec;
    tv->tv_usec = ts->tv_nsec / 1000;
}

static int timeval_cmp(struct timeval *x, struct timeval *y)
{
    if (x->tv_sec > y->tv_sec)
        return 1;
    if (x->tv_sec < y->tv_sec)
        return -1;
    if (x->tv_usec > y->tv_usec)
        return 1;
    if (x->tv_usec < y->tv_usec)
        return -1;
    return 0;
}

void comdb2_nanosleep(struct timespec *req)
{
    struct timeval before, now, need, elapsed;
    timespec_to_timeval(req, &need);
    gettimeofday(&before, NULL);
    do {
        sched_yield();
        gettimeofday(&now, NULL);
        timeval_diff(&before, &now, &elapsed);
    } while (timeval_cmp(&elapsed, &need) < 0);
}

void net_delay(const char *host)
{
    int delay = debug_switch_net_delay();
    if (delay) {
        if (delay > net_delay_max)
            return;
        int other_room;
        struct timespec req;
        time_t sec;
        other_room = getroom_callback(NULL, host);
        if (gbl_myroom == other_room)
            return;
        sec = 0;
        if (delay >= net_delay_mult) {
            sec = delay / net_delay_mult;
            delay = delay % net_delay_mult;
        }
        req.tv_sec = sec;
        req.tv_nsec = delay * 100000; // 0.1 ms -> ns

#ifdef _LINUX_SOURCE
        // spin for delay < 10ms
        if (delay < 100)
            comdb2_nanosleep(&req);
        else
#endif
            nanosleep(&req, NULL);
        ++net_delayed;
    }
}

static ssize_t write_stream(netinfo_type *netinfo_ptr,
                            host_node_type *hostinfo_ptr, SBUF2 *sb,
                            void *inptr, size_t maxbytes)
{
    int nwrite = sbuf2write(inptr, maxbytes, sb);

#if 0
   printf("OUT %s:%d %d :\n", from, line, maxbytes);
   fsnapf(stdout, inptr, maxbytes);
#endif

    /* Note for future - this locking seems OTT.  We must already be under lock
     * here or we couldn't safely use the sbuf. */
    if (nwrite > 0) {
        /* update these stats without lock */
        netinfo_ptr->stats.bytes_written += nwrite;
        if (hostinfo_ptr)
            hostinfo_ptr->stats.bytes_written += nwrite;
    }

    return nwrite;
}

#if WITH_SSL
extern ssl_mode gbl_rep_ssl_mode;
extern SSL_CTX *gbl_ssl_ctx;
#endif
static int read_connect_message(SBUF2 *sb, char hostname[], int hostnamel,
                                int *portnum, netinfo_type *netinfo_ptr)
{
    connect_message_type connect_message;
    uint8_t conndata[NET_CONNECT_MESSAGE_TYPE_LEN], *p_buf, *p_buf_end;
    int rc;
    int hosteq = 0;
    char my_hostname[256];
    char to_hostname[256];
    int namelen;

    rc = read_stream(netinfo_ptr, NULL, sb, &conndata,
                     NET_CONNECT_MESSAGE_TYPE_LEN);
    if (rc != NET_CONNECT_MESSAGE_TYPE_LEN)
        return -1;

    p_buf = conndata;
    p_buf_end = (conndata + sizeof(conndata));

    if (!(net_connect_message_get(&connect_message, p_buf, p_buf_end))) {
        return -1;
    }

    /* If the hostname doesn't fit in HOSTNAME_LEN (16) characters,
     * the first byte of host will be '.' followed by the name length,
     * and the real hostname follows. */
    if (connect_message.my_hostname[0] == '.') {
        connect_message.my_hostname[HOSTNAME_LEN - 1] = 0;
        namelen = atoi(&connect_message.my_hostname[1]);
        if (namelen < 0 || namelen > sizeof(my_hostname)) {
            logmsg(LOGMSG_WARN, "Invalid hostname length %d\n", namelen);
            return 1;
        }
        rc = read_stream(netinfo_ptr, NULL, sb, my_hostname, namelen);
        if (rc != namelen)
            return -1;
    } else {
        strncpy0(my_hostname, connect_message.my_hostname, HOSTNAME_LEN);
    }

    if (connect_message.to_hostname[0] == '.') {
        connect_message.to_hostname[HOSTNAME_LEN - 1] = 0;
        namelen = atoi(&connect_message.to_hostname[1]);
        if (namelen < 0 || namelen > sizeof(to_hostname)) {
            logmsg(LOGMSG_WARN, "Invalid hostname length %d\n", namelen);
            return 1;
        }
        rc = read_stream(netinfo_ptr, NULL, sb, to_hostname, namelen);
        if (rc != namelen)
            return -1;
    } else {
        strncpy0(to_hostname, connect_message.to_hostname, HOSTNAME_LEN);
    }

    if (strcmp(netinfo_ptr->myhostname, to_hostname) == 0)
        hosteq = 1;

    if ((!hosteq) || ((netinfo_ptr->myport != connect_message.to_portnum))) {
        logmsg(LOGMSG_ERROR,
               "netinfo_ptr->hostname = %s, "
               "connect_message.to_hostname = %s\n",
               netinfo_ptr->myhostname, to_hostname);
        logmsg(LOGMSG_ERROR, 
                "netinfo_ptr->myport != connect_message.to_portnum %d %d\n",
                netinfo_ptr->myport, connect_message.to_portnum);
        logmsg(LOGMSG_ERROR, "origin: from=hostname=%s node=%d port=%d\n",
               my_hostname, connect_message.my_nodenum,
               connect_message.my_portnum);
        logmsg(LOGMSG_ERROR, "service: %s\n", netinfo_ptr->service);

        return -1;
    }

    if (netinfo_ptr->allow_rtn &&
        !netinfo_ptr->allow_rtn(netinfo_ptr, intern(my_hostname))) {
        logmsg(LOGMSG_ERROR,
               "received connection from node %d, hostname %s which is not "
               "allowed\n",
               connect_message.my_nodenum, my_hostname);
        return -2;
    }

    strncpy(hostname, my_hostname, hostnamel);
    *portnum = connect_message.my_portnum;

#if WITH_SSL
    if (connect_message.flags & CONNECT_MSG_SSL) {
        if (gbl_rep_ssl_mode < SSL_ALLOW) {
            /* Reject if mis-configured. */
            logmsg(LOGMSG_ERROR,
                   "Misconfiguration: Peer requested SSL, "
                   "but I don't have an SSL key pair.\n");
            return -1;
        }

        rc = sslio_accept(sb, gbl_ssl_ctx, gbl_rep_ssl_mode, gbl_dbname,
                          gbl_nid_dbname, NULL, 0, 1);
        if (rc != 1)
            return -1;
    } else if (gbl_rep_ssl_mode >= SSL_REQUIRE) {
        /* Reject if I require SSL. */
        logmsg(LOGMSG_WARN,
               "Replicant SSL connections are required.\n");
        return -1;
    }
#else
    if (connect_message.flags & CONNECT_MSG_SSL) {
        logmsg(LOGMSG_ERROR, "Misconfiguration: Peer requested SSL, "
                             "but I am not built with SSL.\n");
        return -1;
    }
#endif

    return 0;
}

static int empty_write_list(host_node_type *host_node_ptr)
{
    write_data *ptr, *nxt;

    Pthread_mutex_lock(&(host_node_ptr->enquelk));

    nxt = ptr = host_node_ptr->write_head;
    while (nxt != NULL) {
        ptr = ptr->next;
        free(nxt);
        nxt = ptr;
    }
    host_node_ptr->write_head = host_node_ptr->write_tail = NULL;

    host_node_ptr->enque_count = 0;
    host_node_ptr->enque_bytes = 0;

    Pthread_mutex_unlock(&(host_node_ptr->enquelk));

    return 0;
}

static int write_connect_message(netinfo_type *netinfo_ptr,
                                 host_node_type *host_node_ptr, SBUF2 *sb)
{
    connect_message_type connect_message;
    uint8_t conndata[NET_CONNECT_MESSAGE_TYPE_LEN], *p_buf, *p_buf_end;
    int rc;
    char type;
    int append_to = 0, append_from = 0;

    type = 0;

    rc = write_stream(netinfo_ptr, host_node_ptr, sb, &type, sizeof(char));
    if (rc != sizeof(char)) {
        host_node_errf(LOGMSG_ERROR, host_node_ptr, "write connect message error\n");
        return 1;
    }

    memset(&connect_message, 0, sizeof(connect_message_type));

    if (host_node_ptr->hostname_len > HOSTNAME_LEN) {
        snprintf(connect_message.to_hostname,
                 sizeof(connect_message.to_hostname), ".%d",
                 host_node_ptr->hostname_len);
        append_to = 1;
    } else {
        strncpy0(connect_message.to_hostname, host_node_ptr->host,
                 sizeof(connect_message.to_hostname));
    }
    connect_message.to_portnum = host_node_ptr->port;
    /* It was `to_nodenum`. */
    connect_message.flags = 0;
#if WITH_SSL
    if (gbl_rep_ssl_mode >= SSL_REQUIRE)
        connect_message.flags |= CONNECT_MSG_SSL;
#endif

    if (netinfo_ptr->myhostname_len > HOSTNAME_LEN) {
        snprintf(connect_message.my_hostname,
                 sizeof(connect_message.my_hostname), ".%d",
                 netinfo_ptr->myhostname_len);
        append_from = 1;
    } else {
        strncpy0(connect_message.my_hostname, netinfo_ptr->myhostname,
                 sizeof(connect_message.my_hostname));
    }
    if (netinfo_ptr->myport)
        connect_message.my_portnum =
            netinfo_ptr->myport | (netinfo_ptr->netnum << 16);
    else if (netinfo_ptr->ischild)
        connect_message.my_portnum =
            netinfo_ptr->parent->myport | (netinfo_ptr->netnum << 16);
    else
        connect_message.my_portnum = 0; /* ? */

    connect_message.my_nodenum = 0;

    p_buf = conndata;
    p_buf_end = (conndata + sizeof(conndata));

    net_connect_message_put(&connect_message, p_buf, p_buf_end);

    /* always do a write_stream for the connect message */
    rc = write_stream(netinfo_ptr, host_node_ptr, sb, &conndata,
                      NET_CONNECT_MESSAGE_TYPE_LEN);
    if (rc != sizeof(connect_message_type)) {
        host_node_errf(LOGMSG_ERROR, host_node_ptr, "write connect message error\n");
        return 1;
    }

    if (append_from) {
        rc = write_stream(netinfo_ptr, host_node_ptr, sb,
                          netinfo_ptr->myhostname, netinfo_ptr->myhostname_len);
        if (rc != netinfo_ptr->myhostname_len) {
            host_node_errf(LOGMSG_ERROR, host_node_ptr,
                           "write connect message error (from)\n");
            return 1;
        }
    }
    if (append_to) {
        rc = write_stream(netinfo_ptr, host_node_ptr, sb, host_node_ptr->host,
                          host_node_ptr->hostname_len);
        if (rc != host_node_ptr->hostname_len) {
            host_node_errf(LOGMSG_ERROR, host_node_ptr, "write connect message error (to)\n");
            return 1;
        }
    }

#if WITH_SSL
    if (gbl_rep_ssl_mode >= SSL_REQUIRE) {
        sbuf2flush(sb);
        if (sslio_connect(sb, gbl_ssl_ctx, gbl_rep_ssl_mode, gbl_dbname,
                          gbl_nid_dbname, NULL, 0, 1) != 1)
            return 1;
    }
#endif

    return 0;
}

/* To reduce double buffering and other daftness this has evolved a sort of
 * writev style interface with data1 and data2. */
static int write_message_int(netinfo_type *netinfo_ptr,
                             host_node_type *host_node_ptr, int type,
                             const struct iovec *iov, int iovcount, int flags)
{
    wire_header_type wire_header;
    int rc;

    if ((flags & WRITE_MSG_NOHELLOCHECK) == 0) {
        if (!host_node_ptr->got_hello) {
            /*
            fprintf(stderr, "%s: to %s, no hello\n",
               __func__, host_node_ptr->host);
            */
            return -9;
        }
    }

    /* The writer thread will fill in these details later.. for now, we don't
     * necessarily know the correct details anyway. */
    /*
    strncpy0(wire_header.fromhost, netinfo_ptr->myhostname,
       sizeof(wire_header.fromhost));
    wire_header.fromport = netinfo_ptr->myport;
    wire_header.fromnode = netinfo_ptr->mynode;
    strncpy0(wire_header.tohost, host_node_ptr->host,
       sizeof(wire_header.tohost));
    wire_header.toport = host_node_ptr->port;
    wire_header.tonode = host_node_ptr->node;
    */

    wire_header.type = type;

    /* Add this message to our linked list to send. */
    rc = write_list(netinfo_ptr, host_node_ptr, &wire_header, iov, iovcount,
                    flags);
    if (rc < 0) {
        if (rc == -1) {
            logmsg(LOGMSG_ERROR, "%s: got reallybad failure?\n", __func__);
            return 2;
        } else {
            return rc;
        }
    }

    /* wake up the writer thread */
    if (flags & WRITE_MSG_NODELAY)
        Pthread_cond_signal(&(host_node_ptr->write_wakeup));

    return 0;
}

static int write_message_checkhello(netinfo_type *netinfo_ptr,
                                    host_node_type *host_node_ptr, int type,
                                    const struct iovec *iov, int iovcount,
                                    int nodelay, int nodrop, int inorder)
{
    return write_message_int(netinfo_ptr, host_node_ptr, type, iov, iovcount,
                             (nodelay ? WRITE_MSG_NODELAY : 0) |
                                 WRITE_MSG_NOHELLOCHECK |
                                 (nodrop ? WRITE_MSG_NOLIMIT : 0) |
                                 (inorder ? WRITE_MSG_INORDER : 0));
}

static int write_message_nohello(netinfo_type *netinfo_ptr,
                                 host_node_type *host_node_ptr, int type,
                                 const void *data, size_t datalen)
{
    struct iovec iov = {(void *)data, datalen};
    return write_message_int(netinfo_ptr, host_node_ptr, type, &iov, 1,
                             WRITE_MSG_NODELAY | WRITE_MSG_NOHELLOCHECK);
}

static int write_message(netinfo_type *netinfo_ptr,
                         host_node_type *host_node_ptr, int type,
                         const void *data, size_t datalen)
{
    struct iovec iov = {(void *)data, datalen};
    return write_message_int(netinfo_ptr, host_node_ptr, type, &iov, 1,
                             WRITE_MSG_NODELAY);
}

static int read_message_header(netinfo_type *netinfo_ptr,
                               host_node_type *host_node_ptr,
                               wire_header_type *wire_header,
                               char fromhost[256], char tohost[256])
{
    int rc;
    wire_header_type tmpheader;
    uint8_t *p_buf, *p_buf_end;
    int namelen;

    rc = read_stream(netinfo_ptr, host_node_ptr, host_node_ptr->sb, &tmpheader,
                     sizeof(wire_header_type));

    if (rc != sizeof(wire_header_type))
        return 1;

    p_buf = (uint8_t *)&tmpheader;
    p_buf_end = ((uint8_t *)&tmpheader + sizeof(wire_header_type));

    net_wire_header_get(wire_header, p_buf, p_buf_end);
    if (wire_header->fromhost[0] == '.') {
        wire_header->fromhost[HOSTNAME_LEN - 1] = 0;
        namelen = atoi(&wire_header->fromhost[1]);
        if (namelen < 1 || namelen > 256)
            return 1;
        rc = read_stream(netinfo_ptr, host_node_ptr, host_node_ptr->sb,
                         fromhost, namelen);
        if (rc != namelen)
            return 1;
    } else {
        strncpy0(fromhost, wire_header->fromhost, HOSTNAME_LEN);
    }
    if (wire_header->tohost[0] == '.') {
        wire_header->tohost[HOSTNAME_LEN - 1] = 0;
        namelen = atoi(&wire_header->tohost[1]);
        if (namelen < 1 || namelen > 256)
            return 1;
        rc = read_stream(netinfo_ptr, host_node_ptr, host_node_ptr->sb, tohost,
                         namelen);
        if (rc != namelen)
            return 1;
    } else {
        strncpy0(tohost, wire_header->tohost, HOSTNAME_LEN);
    }

    return 0;
}

static int write_heartbeat(netinfo_type *netinfo_ptr,
                           host_node_type *host_node_ptr)
{
    /* heartbeats always jump to the head */
    return write_message_int(netinfo_ptr, host_node_ptr, WIRE_HEADER_HEARTBEAT,
                             NULL, 0,
                             WRITE_MSG_HEAD | WRITE_MSG_NODUPE |
                                 WRITE_MSG_NODELAY | WRITE_MSG_NOLIMIT);
}

/*
  this is the protocol where each node advertises all the other nodes
  they know about so that eventually (quickly) every node know about
  every other nodes
*/
static int write_hello(netinfo_type *netinfo_ptr, host_node_type *host_node_ptr)
{
    int rc;
    int numhosts;
    char *data;
    uint8_t *p_buf, *p_buf_end;
    host_node_type *tmp_host_ptr;
    int datasz;

#ifdef DEBUG
    fprintf(stderr, "sending hello to node %s\n", host_node_ptr->host);
#endif

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    numhosts = 0;
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next)
        numhosts++;

    datasz = sizeof(int) + sizeof(int) + /* int numhosts */
             (HOSTNAME_LEN * numhosts) + /* char host[16]... ( 1 per host ) */
             (sizeof(int) * numhosts) +  /* int port...      ( 1 per host ) */
             (sizeof(int) * numhosts) +  /* int node...      ( 1 per host ) */
             (8 * numhosts);             /* some fluff space */

    /* write long hostnames */
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        if (tmp_host_ptr->hostname_len > HOSTNAME_LEN)
            datasz += tmp_host_ptr->hostname_len;
    }
    data = HOST_MALLOC(host_node_ptr, datasz);
    memset(data, 0, datasz);

    p_buf = (uint8_t *)data;
    p_buf_end = (uint8_t *)(data + datasz);

    p_buf = buf_put(&datasz, sizeof(int), p_buf, p_buf_end);

    /* fill in numhosts */
    p_buf = buf_put(&numhosts, sizeof(int), p_buf, p_buf_end);

    /* fill in hostnames */
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        if (tmp_host_ptr->hostname_len > HOSTNAME_LEN) {
            char lenstr[HOSTNAME_LEN] = {0};
            snprintf(lenstr, sizeof(lenstr), ".%d", tmp_host_ptr->hostname_len);
            lenstr[HOSTNAME_LEN - 1] = 0;
            p_buf = buf_no_net_put(lenstr, HOSTNAME_LEN - 1, p_buf, p_buf_end);
        } else {
            p_buf = buf_no_net_put(tmp_host_ptr->host, HOSTNAME_LEN - 1, p_buf,
                                   p_buf_end);
        }

        /* null terminate */
        p_buf = buf_zero_put(sizeof(char), p_buf, p_buf_end);
    }

    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        p_buf = buf_put(&tmp_host_ptr->port, sizeof(int), p_buf, p_buf_end);
    }

    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        int node = machine_num(tmp_host_ptr->host);
        p_buf = buf_put(&node, sizeof(int), p_buf, p_buf_end);
    }
    /* write long hostnames */
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        if (tmp_host_ptr->hostname_len > HOSTNAME_LEN)
            p_buf =
                buf_no_net_put(tmp_host_ptr->host, tmp_host_ptr->hostname_len,
                               p_buf, p_buf_end);
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    rc = write_message_nohello(netinfo_ptr, host_node_ptr, WIRE_HEADER_HELLO,
                               data, datasz);

    free(data);

    return rc;
}

static int write_hello_reply(netinfo_type *netinfo_ptr,
                             host_node_type *host_node_ptr)
{
    int rc;
    int numhosts;
    char *data;
    uint8_t *p_buf, *p_buf_end;
    host_node_type *tmp_host_ptr;
    int datasz;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    numhosts = 0;
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next)
        numhosts++;


    datasz = sizeof(int) + sizeof(int) + /* int numhosts */
             (HOSTNAME_LEN * numhosts) + /* char host[16]... ( 1 per host ) */
             (sizeof(int) * numhosts) +  /* int port...      ( 1 per host ) */
             (sizeof(int) * numhosts) +  /* int node...      ( 1 per host ) */
             (8 * numhosts);             /* some fluff space */

    /* write long hostnames */
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        if (tmp_host_ptr->hostname_len > HOSTNAME_LEN)
            datasz += tmp_host_ptr->hostname_len;
    }
    data = HOST_MALLOC(host_node_ptr, datasz);
    memset(data, 0, datasz);

    p_buf = (uint8_t *)data;
    p_buf_end = ((uint8_t *)data + datasz);

    /* fill in datasz */
    p_buf = buf_put(&datasz, sizeof(datasz), p_buf, p_buf_end);

    /* fill in numhosts */
    p_buf = buf_put(&numhosts, sizeof(numhosts), p_buf, p_buf_end);

    /* fill in hostnames */
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        if (tmp_host_ptr->hostname_len > HOSTNAME_LEN) {
            char lenstr[HOSTNAME_LEN] = {0};
            snprintf(lenstr, sizeof(lenstr), ".%d", tmp_host_ptr->hostname_len);
            lenstr[HOSTNAME_LEN - 1] = 0;
            p_buf = buf_no_net_put(lenstr, HOSTNAME_LEN - 1, p_buf, p_buf_end);
        } else
            p_buf = buf_no_net_put(tmp_host_ptr->host, HOSTNAME_LEN - 1, p_buf,
                                   p_buf_end);

        /* null terminate */
        p_buf = buf_zero_put(sizeof(char), p_buf, p_buf_end);
    }

    /* fill in port numbers */
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        p_buf = buf_put(&(tmp_host_ptr->port), sizeof(int), p_buf, p_buf_end);
    }

    /* fill in node numbers */
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        int node = machine_num(tmp_host_ptr->host);
        p_buf = buf_put(&node, sizeof(int), p_buf, p_buf_end);
    }

    /* write long hostnames */
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        if (tmp_host_ptr->hostname_len > HOSTNAME_LEN)
            p_buf =
                buf_no_net_put(tmp_host_ptr->host, tmp_host_ptr->hostname_len,
                               p_buf, p_buf_end);
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    rc = write_message_nohello(netinfo_ptr, host_node_ptr,
                               WIRE_HEADER_HELLO_REPLY, data, datasz);

    free(data);

    return rc;
}

static void add_millisecs_to_timespec(struct timespec *orig, int millisecs)
{
    int nanosecs = orig->tv_nsec;
    int secs = orig->tv_sec;

    secs += (millisecs / 1000);
    millisecs = (millisecs % 1000);

    nanosecs += (millisecs * MILLION);
    secs += (nanosecs / BILLION);
    nanosecs = (nanosecs % BILLION);
    orig->tv_sec = secs;
    orig->tv_nsec = nanosecs;
    return;
}

static seq_data *add_seqnum_to_waitlist(host_node_type *host_node_ptr,
                                        int seqnum)
{
    seq_data *new_seq_node, *seq_list_ptr;
    new_seq_node = HOST_MALLOC(host_node_ptr, sizeof(seq_data));
    new_seq_node->seqnum = seqnum;
    new_seq_node->ack = 0;
    new_seq_node->outrc = 0;
    new_seq_node->next = NULL;
    new_seq_node->payload = NULL;
    new_seq_node->payloadlen = 0;
    new_seq_node->timestamp = time(NULL);
    /* always add to the end of the list. */
    /* only remove from the beginning, and then,
       only if the "ack" has occurred */
    if (host_node_ptr->wait_list == NULL)
        host_node_ptr->wait_list = new_seq_node;
    else {
        seq_list_ptr = host_node_ptr->wait_list;
        while (seq_list_ptr->next != NULL)
            seq_list_ptr = seq_list_ptr->next;
        seq_list_ptr->next = new_seq_node;
    }
    return new_seq_node;
}

/* already under lock */
static int remove_seqnum_from_waitlist(host_node_type *host_node_ptr,
                                       void **payloadptr, int *payloadlen,
                                       int seqnum)
{
    seq_data *seq_list_ptr, *back;
    int outrc;

    back = seq_list_ptr = host_node_ptr->wait_list;

    while (seq_list_ptr != NULL && seq_list_ptr->seqnum != seqnum) {
        back = seq_list_ptr;
        seq_list_ptr = seq_list_ptr->next;
    }
    if (seq_list_ptr == NULL) {
        /*fprintf(stderr,"cant find seq num %d in waitlist\n", seqnum);*/
        return -1;
    }
    if (seq_list_ptr == host_node_ptr->wait_list)
        host_node_ptr->wait_list = host_node_ptr->wait_list->next;
    else
        back->next = seq_list_ptr->next;

    outrc = seq_list_ptr->outrc;
    if (payloadptr) {
        (*payloadptr) = seq_list_ptr->payload;
        seq_list_ptr->payload = NULL;
        (*payloadlen) = seq_list_ptr->payloadlen;
    }
    if (seq_list_ptr->payload)
        free(seq_list_ptr->payload);
    free(seq_list_ptr);

    return outrc;
}

static void net_throttle_wait_loop(netinfo_type *netinfo_ptr,
                                   host_node_type *host_ptr,
                                   uint32_t queue_threshold,
                                   uint64_t byte_threshold)
{
    int loops = 0;
    Pthread_mutex_lock(&(host_ptr->throttle_lock));
    host_ptr->throttle_waiters++;

    while (!host_ptr->closed && ((host_ptr->enque_count > queue_threshold) ||
                                 (host_ptr->enque_bytes > byte_threshold)))

    {
        struct timespec waittime;
        struct timeval tv;

#ifdef HAS_CLOCK_GETTIME
        clock_gettime(CLOCK_REALTIME, &waittime);
#else
        gettimeofday(&tv, NULL);
        timeval_to_timespec(&tv, &waittime);
#endif
        add_millisecs_to_timespec(&waittime, 1000);

        if (loops > 0) {
            logmsg(LOGMSG_ERROR, "%s thread %lu waiting for net count to drop"
                                 " to %u enqueued buffers or %lu bytes (%d "
                                 "loops)\n",
                   __func__, pthread_self(), queue_threshold, byte_threshold,
                   loops);
        }

        host_ptr->stats.throttle_waits++;
        netinfo_ptr->stats.throttle_waits++;

        pthread_cond_timedwait(&(host_ptr->throttle_wakeup),
                               &(host_ptr->throttle_lock), &waittime);

        loops++;
    }

    host_ptr->throttle_waiters--;
    Pthread_mutex_unlock(&(host_ptr->throttle_lock));
}


int net_throttle_wait(netinfo_type *netinfo_ptr)
{
    uint32_t queue_threshold;
    uint64_t byte_threshold;
    int cnt = 0;

    /* let one message always get in */

    queue_threshold =
        (netinfo_ptr->throttle_percent * netinfo_ptr->max_queue) / 100;

    byte_threshold =
        (netinfo_ptr->throttle_percent * netinfo_ptr->max_bytes) / 100;

    if (netinfo_ptr->fake || queue_threshold >= netinfo_ptr->max_queue ||
        queue_threshold == 0 || byte_threshold >= netinfo_ptr->max_bytes ||
        byte_threshold == 0)
        return 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    host_node_type *ptr = netinfo_ptr->head;
    /* let 1 message always slip in */
    if (ptr && ptr->enque_count) {
        while (ptr) {
            if (!ptr->closed && ((ptr->enque_count > queue_threshold) ||
                                 (ptr->enque_bytes > byte_threshold))) {
                cnt++;
                net_throttle_wait_loop(netinfo_ptr, ptr, queue_threshold,
                                       byte_threshold);
            }
            ptr = ptr->next;
        }
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
    return cnt;
}

int net_get_queue_size(netinfo_type *netinfo_ptr, const char *hostname,
                       int *limit, int *usage)
{

    host_node_type *host_node_ptr = NULL;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    host_node_ptr = get_host_node_by_name_ll(netinfo_ptr, hostname);
    if (host_node_ptr == NULL) {
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        return NET_SEND_FAIL_INVALIDNODE;
    }

    Pthread_mutex_lock(&(host_node_ptr->enquelk));
    *usage = host_node_ptr->enque_count;
    *limit = netinfo_ptr->max_queue;
    Pthread_mutex_unlock(&(host_node_ptr->enquelk));

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return 0;
}

int net_send_message_payload_ack(netinfo_type *netinfo_ptr, const char *to_host,
                                 int usertype, void *data, int datalen,
                                 uint8_t **payloadptr, int *payloadlen,
                                 int waitforack, int waitms)
{
    net_send_message_header tmphd, msghd;
    uint8_t *p_buf, *p_buf_end;
    seq_data *seq_ptr;
    host_node_type *host_node_ptr;
    int rc;
    struct timespec waittime;
#ifndef HAS_CLOCK_GETTIME
    struct timeval tv;
#endif
    struct iovec iov[2];

    rc = 0;

    /* do nothing if we have a fake netinfo */
    if (netinfo_ptr->fake)
        return 0;
    if (to_host == NULL)
        abort();

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    host_node_ptr = get_host_node_by_name_ll(netinfo_ptr, to_host);
    if (host_node_ptr == NULL) {
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        return NET_SEND_FAIL_INVALIDNODE;
    }

    if (host_node_ptr->host == netinfo_ptr->myhostname) {
        rc = NET_SEND_FAIL_SENDTOME;
        goto end;
    }

    /* fail if we don't have a socket */
    if (host_node_ptr->fd == -1) {
        rc = NET_SEND_FAIL_NOSOCK;
        goto end;
    }

    /* fail if we are closed */
    if (host_node_ptr->closed) {
        rc = NET_SEND_FAIL_CLOSED;
        goto end;
    }

    msghd.usertype = usertype;
    Pthread_mutex_lock(&(netinfo_ptr->seqlock));
    msghd.seqnum = ++netinfo_ptr->seqnum;
    Pthread_mutex_unlock(&(netinfo_ptr->seqlock));
    msghd.waitforack = waitforack;
    msghd.datalen = datalen;

    p_buf = (uint8_t *)&tmphd;
    p_buf_end = ((uint8_t *)&tmphd + sizeof(net_send_message_header));

    net_send_message_header_put(&msghd, p_buf, p_buf_end);

    iov[0].iov_base = (int8_t *)&tmphd;
    iov[0].iov_len = sizeof(tmphd);
    iov[1].iov_base = data;
    iov[1].iov_len = datalen;

    Pthread_mutex_lock(&(host_node_ptr->wait_mutex));

    if (waitforack) {
        seq_ptr = add_seqnum_to_waitlist(host_node_ptr, msghd.seqnum);
        seq_ptr->ack = 0;
    } else
        seq_ptr = NULL;

    rc = write_message_checkhello(netinfo_ptr, host_node_ptr,
                                  WIRE_HEADER_USER_MSG, iov, 2, 1 /*nodelay*/,
                                  0, 0);

    if (rc != 0) {
        if (seq_ptr)
            remove_seqnum_from_waitlist(host_node_ptr, (void**) payloadptr, 
                                        payloadlen, seq_ptr->seqnum);
        Pthread_mutex_unlock(&(host_node_ptr->wait_mutex));

        rc = NET_SEND_FAIL_WRITEFAIL;
        goto end;
    }

    if (!waitforack) {
        Pthread_mutex_unlock(&(host_node_ptr->wait_mutex));
        rc = 0;
        goto end;
    }

#ifdef HAS_CLOCK_GETTIME
    rc = clock_gettime(CLOCK_REALTIME, &waittime);
    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "clock_gettime err %d %s\n", errno, strerror(errno));
        remove_seqnum_from_waitlist(host_node_ptr, payloadptr, payloadlen,
                                    seq_ptr->seqnum);
        Pthread_mutex_unlock(&(host_node_ptr->wait_mutex));

        rc = NET_SEND_FAIL_INTERNAL;
        goto end;
    }
#else
    rc = gettimeofday(&tv, NULL);
    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "gettimeofday failed\n");
        remove_seqnum_from_waitlist(host_node_ptr, (void**) payloadptr, payloadlen, 
                                    seq_ptr->seqnum);
        Pthread_mutex_unlock(&(host_node_ptr->wait_mutex));

        rc = NET_SEND_FAIL_INTERNAL;
        goto end;
    }

    timeval_to_timespec(&tv, &waittime);
#endif

    add_millisecs_to_timespec(&waittime, waitms);

    rc = 0;
    while (1) {
        if (seq_ptr->ack == 1) {
            rc = remove_seqnum_from_waitlist(host_node_ptr, (void**) payloadptr,
                                             payloadlen, seq_ptr->seqnum);
            /* user is only allowed to return >=0 */
            if (rc < 0)
                rc = NET_SEND_FAIL_INVALIDACKRC;

            Pthread_mutex_unlock(&(host_node_ptr->wait_mutex));

            if (rc == ETIMEDOUT) {
                logmsg(LOGMSG_ERROR, "timeout, but found reply afterwards??\n");
            }
            goto end;
        }

        if (rc == ETIMEDOUT) {
            remove_seqnum_from_waitlist(host_node_ptr, (void**) payloadptr,
                                        payloadlen, seq_ptr->seqnum);
            logmsg(LOGMSG_ERROR, "net_send_message: timeout to %s\n",
                    host_node_ptr->host);

            Pthread_mutex_unlock(&(host_node_ptr->wait_mutex));

            rc = NET_SEND_FAIL_TIMEOUT;
            goto end;
        } else if (rc != 0) {
            remove_seqnum_from_waitlist(host_node_ptr, (void**) payloadptr,
                                        payloadlen, seq_ptr->seqnum);
            Pthread_mutex_unlock(&(host_node_ptr->wait_mutex));
            logmsg(LOGMSG_ERROR,
                   "net_send_message: host %s, "
                   "got rc = %d from pthread_cond_timedwait\n",
                   host_node_ptr->host, rc);

            rc = NET_SEND_FAIL_INTERNAL;
            goto end;
        }

        /*
        fprintf(stderr, "waiting for ack from %s\n", host_node_ptr->host);
        */

        rc = pthread_cond_timedwait(&(host_node_ptr->ack_wakeup),
                                    &(host_node_ptr->wait_mutex), &waittime);

        if (rc == EINVAL)
            goto end;

        /*fprintf(stderr, "got ack\n");*/
    }

end:
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return rc;
}

int net_send_message(netinfo_type *netinfo_ptr, const char *to_host,
                     int usertype, void *data, int datalen, int waitforack,
                     int waitms)
{
    return net_send_message_payload_ack(netinfo_ptr, to_host, usertype, data,
                                        datalen, NULL, NULL, waitforack,
                                        waitms);
}


static unsigned long long num_flushes = 0;
static unsigned long long send_interval_flushes = 0;
static unsigned long long explicit_flushes = 0;

unsigned long long net_get_send_interval_flushes(void)
{
    return send_interval_flushes;
}

void net_reset_send_interval_flushes(void) { send_interval_flushes = 0; }

unsigned long long net_get_explicit_flushes(void) { return explicit_flushes; }

void net_reset_explicit_flushes(void) { explicit_flushes = 0; }

unsigned long long net_get_num_flushes(void) { return num_flushes; }

void net_reset_num_flushes(void) { num_flushes = 0; }

static int stack_flush_min = 50;
int explicit_flush_trace = 0;

void net_enable_explicit_flush_trace(void) { explicit_flush_trace = 1; }

void net_disable_explicit_flush_trace(void) { explicit_flush_trace = 0; }

void comdb2_cheapstack(FILE *f);

static void net_trace_explicit_flush(void)
{
    static int lastpr = 0, count = 0;
    int now, flushmin = stack_flush_min;

    if (!explicit_flush_trace)
        return;

    count++;

    if (flushmin > 0 && (now = comdb2_time_epoch()) - lastpr) {
        if (count > flushmin) {
            comdb2_cheapstack(stdout);
        }
        lastpr = now;
        count = 0;
    }
}

int net_get_stack_flush_threshold(void) { return stack_flush_min; }

void net_set_stack_flush_threshold(int thresh) { stack_flush_min = thresh; }

int gbl_dump_full_net_queue = 0;

static void dump_queue(netinfo_type *netinfo_ptr, host_node_type *host_node_ptr)
{
    int now, cnt = 0, logput_cnt = 0, non_logput_cnt = 0;

    if (netinfo_ptr->getlsn_rtn == NULL)
        return;

    if ((now = time(NULL)) - host_node_ptr->last_queue_dump) {
        write_data *ptr;
        int file, offset, rc, wl = 0;
        logmsg(LOGMSG_USER, "Dumping net-queue for %s\n", host_node_ptr->host);
        Pthread_mutex_lock(&(host_node_ptr->enquelk));
        ptr = host_node_ptr->write_head;
        while (ptr != NULL) {
            cnt++;
            if ((rc = (netinfo_ptr->getlsn_rtn)(netinfo_ptr, ptr->payload.raw,
                                                ptr->len, &file, &offset)) ==
                0) {
                logput_cnt++;
                if (wl == 0) {
                    logmsg(LOGMSG_USER, "%s: ", host_node_ptr->host);
                }
                logmsg(LOGMSG_USER, "%d:%d ", file, offset);
                wl = 1;
                if ((logput_cnt % 20) == 0) {
                    logmsg(LOGMSG_USER, "\n");
                    wl = 0;
                }
            } else {
                non_logput_cnt++;
            }
            ptr = ptr->next;
        }
        Pthread_mutex_unlock(&(host_node_ptr->enquelk));

        if (wl) {
            logmsg(LOGMSG_USER, "\n");
        }
        logmsg(LOGMSG_USER, "%s: %d logputs, %d other\n", host_node_ptr->host,
               logput_cnt, non_logput_cnt);
        host_node_ptr->last_queue_dump = now;
    }
}

static int net_send_int(netinfo_type *netinfo_ptr, const char *host,
                        int usertype, void *data, int datalen, int nodelay,
                        int numtails, void **tails, int *taillens, int nodrop,
                        int inorder, int trace)
{
    host_node_type *host_node_ptr;
    net_send_message_header tmphd, msghd;
    uint8_t *p_buf, *p_buf_end;
    int rc;
    struct iovec iov[35];
    int iovcount;
    int total_tails_len = 0;
    int i;
    int tailen;
#if 0
   if (strcmp(netinfo_ptr->service, "offloadsql") == 0) {
       printf("net %s usertype %d to %s\n", netinfo_ptr->service, usertype, host);
   }
#endif
#ifdef UDP_DEBUG
    if (usertype == 2) {
        int last = __atomic_exchange_n(&curr_udp_cnt, 0, __ATOMIC_SEQ_CST);
        if (last > 0)
            logmsg(LOGMSG_USER, "udp_packets sent %d\n", last);
    }
#endif

    rc = 0;
    if (numtails > 32) {
        logmsg(LOGMSG_ERROR, "too many tails %d passed to net_send_tails, max 32\n",
               numtails);
        return -1;
    }

    /* testpoint- throw 'queue-full' errors */
    if ((0 == rc) && (NET_TEST_QUEUE_FULL == netinfo_ptr->net_test) &&
        (rand() % 100) == 0) {
        logmsg(LOGMSG_INFO, "%s line %d debug/random QUEUE-FULL\n", __func__,
               __LINE__);
        return NET_SEND_FAIL_QUEUE_FULL;
    }

    /* do nothing if we have a fake netinfo */
    if (netinfo_ptr->fake)
        return 0;

    for (i = 0; i < numtails; i++)
        total_tails_len += taillens[i];

    tailen =
        (numtails > 0 && tails && total_tails_len > 0) ? total_tails_len : 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    host_node_ptr = get_host_node_by_name_ll(netinfo_ptr, host);
    if (host_node_ptr == NULL) {
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        if (trace) {
            logmsg(LOGMSG_USER, "%s line %d returning INVALIDNODE\n", __func__,
                   __LINE__);
        }
        return NET_SEND_FAIL_INVALIDNODE;
    }

    if (host_node_ptr->host == netinfo_ptr->myhostname) {
        if (trace) {
            logmsg(LOGMSG_USER, "%s line %d returning FAIL_SENDTOME\n",
                   __func__, __LINE__);
        }
        rc = NET_SEND_FAIL_SENDTOME;
        goto end;
    }

    /* fail if we don't have a socket */
    if (host_node_ptr->fd == -1) {
        if (trace) {
            logmsg(LOGMSG_USER, "%s line %d returning NOSOCK\n", __func__,
                   __LINE__);
        }
        rc = NET_SEND_FAIL_NOSOCK;
        goto end;
    }

    /* fail if we are closed */
    if (host_node_ptr->closed) {
        if (trace) {
            logmsg(LOGMSG_USER, "%s line %d returning CLOSED\n", __func__,
                   __LINE__);
        }
        rc = NET_SEND_FAIL_CLOSED;
        goto end;
    }

    host_node_ptr->num_sends++;
    if (nodelay) {
        explicit_flushes++;
        net_trace_explicit_flush();
    } else if (host_node_ptr->num_sends > netinfo_ptr->enque_flush_interval) {
        send_interval_flushes++;
        nodelay = 1;
    }

    if (nodelay)
        host_node_ptr->num_sends = 0;

    /*ctrace("net_send_message: to node %s, ut=%d\n", host_node_ptr->host, usertype);*/

    msghd.usertype = usertype;
    Pthread_mutex_lock(&(netinfo_ptr->seqlock));
    msghd.seqnum = ++netinfo_ptr->seqnum;
    Pthread_mutex_unlock(&(netinfo_ptr->seqlock));
    msghd.waitforack = 0;
    msghd.datalen = datalen + tailen;

    p_buf = (uint8_t *)&tmphd;
    p_buf_end = ((uint8_t *)&tmphd + sizeof(net_send_message_header));

    net_send_message_header_put(&msghd, p_buf, p_buf_end);

    iov[0].iov_base = (int8_t *)&tmphd;
    iov[0].iov_len = sizeof(tmphd);
    iovcount = 1;
    if (data && datalen) {
        iov[iovcount].iov_base = data;
        iov[iovcount].iov_len = datalen;
        iovcount++;
    }
    if (numtails > 0) {
        for (i = 0; i < numtails; i++) {
            iov[iovcount].iov_base = tails[i];
            iov[iovcount].iov_len = taillens[i];
            iovcount++;
        }
    }

    if (nodelay) {
        host_node_ptr->num_flushes++;
        num_flushes++;
    }

    rc = write_message_checkhello(netinfo_ptr, host_node_ptr,
                                  WIRE_HEADER_USER_MSG, iov, iovcount, nodelay,
                                  nodrop, inorder);

    /* queue is full */
    if (-2 == rc) {
        if (trace) {
            logmsg(LOGMSG_USER, "%s line %d returning QUEUE-FULL\n", __func__,
                   __LINE__);
        }
        rc = NET_SEND_FAIL_QUEUE_FULL;
    }

    /* write_list failed to malloc */
    else if (2 == rc) {
        if (trace) {
            logmsg(LOGMSG_USER, "%s line %d returning MALLOC-FAIL\n", __func__,
                   __LINE__);
        }
        rc = NET_SEND_FAIL_MALLOC_FAIL;
    }

    /* all other failures */
    else if (0 != rc) {
        if (trace) {
            logmsg(LOGMSG_USER, "%s line %d returning WRITEFAIL\n", __func__,
                   __LINE__);
        }
        rc = NET_SEND_FAIL_WRITEFAIL;
    }

end:

    if (rc == NET_SEND_FAIL_QUEUE_FULL && gbl_dump_full_net_queue)
        dump_queue(netinfo_ptr, host_node_ptr);

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
    return rc;
}

int net_send_authcheck_all(netinfo_type *netinfo_ptr)
{
    int rc, count = 0, i;
    const char *nodes[REPMAX];
    int outrc = 0;

    count = net_get_all_nodes(netinfo_ptr, nodes);

    for (i = 0; i < count; i++) {
        rc = net_send_message(netinfo_ptr, nodes[i], NET_AUTHENTICATION_CHECK,
                              NULL, 0, 0, 5000);
        if (rc < 0) {
            logmsg(LOGMSG_ERROR,
                   "Sending Auth Check failed for node %s rc=%d\n", nodes[i],
                   rc);
            outrc++;
        }
    }
    return outrc;
}


/* Re-order this on the queue */
int net_send_inorder(netinfo_type *netinfo_ptr, const char *host, int usertype,
                     void *data, int datalen, int nodelay)
{
    return net_send_int(netinfo_ptr, host, usertype, data, datalen, nodelay, 0,
                        NULL, 0, 0, 1, 0);
}

int net_send_inorder_nodrop(netinfo_type *netinfo_ptr, const char *host,
                            int usertype, void *data, int datalen, int nodelay)
{
    return net_send_int(netinfo_ptr, host, usertype, data, datalen, nodelay, 0,
                        NULL, 0, 1, 1, 0);
}

int net_send_flags(netinfo_type *netinfo_ptr, const char *host, int usertype,
                   void *data, int datalen, uint32_t flags)
{
    return net_send_int(netinfo_ptr, host, usertype, data, datalen,
                        (flags & NET_SEND_NODELAY), 0, NULL, 0,
                        (flags & NET_SEND_NODROP), (flags & NET_SEND_INORDER),
                        (flags & NET_SEND_TRACE));
}

int net_send(netinfo_type *netinfo_ptr, const char *host, int usertype,
             void *data, int datalen, int nodelay)
{

    return net_send_int(netinfo_ptr, host, usertype, data, datalen, nodelay, 0,
                        NULL, 0, 0, 0, 0);
}

int net_send_nodrop(netinfo_type *netinfo_ptr, const char *host, int usertype,
                    void *data, int datalen, int nodelay)
{

    return net_send_int(netinfo_ptr, host, usertype, data, datalen, nodelay, 0,
                        NULL, 0, 1, 0, 0);
}

int net_send_tails(netinfo_type *netinfo_ptr, const char *host, int usertype,
                   void *data, int datalen, int nodelay, int numtails,
                   void **tails, int *taillens)
{

    return net_send_int(netinfo_ptr, host, usertype, data, datalen, nodelay,
                        numtails, tails, taillens, 0, 0, 0);
}

int net_send_tail(netinfo_type *netinfo_ptr, const char *host, int usertype,
                  void *data, int datalen, int nodelay, void *tail, int tailen)
{

#ifdef _BLOCKSQL_DBG
    int i = 0;
    printf("Sending data [%d]:\n", datalen);
    for (i = 0; i < datalen; i++)
        printf("%02x ", ((char *)data)[i]);
    printf("\n");

    printf("Sending tail[%d]:\n", tailen);
    for (i = 0; i < tailen; i++)
        printf("%02x ", ((char *)tail)[i]);
    printf("\n");
#endif
    return net_send_int(netinfo_ptr, host, usertype, data, datalen, nodelay, 1,
                        &tail, &tailen, 0, 0, 0);
}

/* returns all nodes MINUS you */
int net_get_all_nodes(netinfo_type *netinfo_ptr, const char *hostlist[REPMAX])
{
    host_node_type *ptr;
    int count = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        /* dont send to yourself */
        if (ptr->host == netinfo_ptr->myhostname)
            continue;

        hostlist[count++] = ptr->host;
        if (count >= REPMAX)
            break;
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return count;
}

int net_get_all_commissioned_nodes(netinfo_type *netinfo_ptr,
                                   const char *hostlist[REPMAX])
{
    host_node_type *ptr;
    int count = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        /* dont send to yourself */
        if (ptr->host == netinfo_ptr->myhostname)
            continue;

        if (!ptr->decom_flag) {
            hostlist[count++] = ptr->host;
            if (count >= REPMAX)
                break;
        }
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return count;
}

int net_get_all_nodes_connected(netinfo_type *netinfo_ptr,
                                const char *hostlist[REPMAX])
{
    host_node_type *ptr;
    int count = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        /* dont send to yourself */
        if (ptr->host == netinfo_ptr->myhostname)
            continue;

        /* dont count disconected guys */
        if (ptr->fd <= 0)
            continue;

        /* dont count guys that didnt hello us */
        if (!ptr->got_hello)
            continue;

        hostlist[count++] = ptr->host;
        if (count >= REPMAX)
            break;
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return count;
}

int net_register_queue_stat(netinfo_type *netinfo_ptr, QSTATINITFP *qinit,
                            QSTATREADERFP *reader, QSTATENQUEFP *enque,
                            QSTATCLEARFP *qclear, QSTATFREEFP *qfree)
{
    host_node_type *tmp_host_ptr;

    /* Set qstat for each existing node */
    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        if (strcmp(tmp_host_ptr->host, netinfo_ptr->myhostname) != 0) {
            tmp_host_ptr->qstat =
                qinit(netinfo_ptr, netinfo_ptr->service, tmp_host_ptr->host);
        }
    }

    netinfo_ptr->qstat_free_rtn = qfree;
    netinfo_ptr->qstat_init_rtn = qinit;
    netinfo_ptr->qstat_reader_rtn = reader;
    netinfo_ptr->qstat_enque_rtn = enque;
    netinfo_ptr->qstat_clear_rtn = qclear;
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return 0;
}

void net_userfunc_iterate(netinfo_type *netinfo_ptr, UFUNCITERFP *uf_iter,
                          void *arg)
{
    for (int i = 0; i <= MAX_USER_TYPE; i++) {
        if (netinfo_ptr->userfuncs[i].func) {
            uf_iter(netinfo_ptr, arg, netinfo_ptr->service,
                    netinfo_ptr->userfuncs[i].name,
                    netinfo_ptr->userfuncs[i].count,
                    netinfo_ptr->userfuncs[i].totus);
        }
    }
}

void net_queue_stat_iterate(netinfo_type *netinfo_ptr, QSTATITERFP qs_iter,
                            void *arg)
{
    host_node_type *tmp_host_ptr;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    for (tmp_host_ptr = netinfo_ptr->head; tmp_host_ptr != NULL;
         tmp_host_ptr = tmp_host_ptr->next) {
        qs_iter(netinfo_ptr, arg, tmp_host_ptr->qstat);
    }
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

int net_register_getlsn(netinfo_type *netinfo_ptr, GETLSNFP func)
{
    netinfo_ptr->getlsn_rtn = func;
    return 0;
}

int net_register_netcmp(netinfo_type *netinfo_ptr, NETCMPFP func)
{
    netinfo_ptr->netcmp_rtn = func;
    return 0;
}

int net_register_hostdown(netinfo_type *netinfo_ptr, HOSTDOWNFP *func)
{
    netinfo_ptr->hostdown_rtn = func;

    return 0;
}

int net_register_name(netinfo_type *netinfo_ptr, char name[])
{
    netinfo_ptr->name = strdup(name);

    return 0;
}

int net_register_hello(netinfo_type *netinfo_ptr, HELLOFP func)
{
    netinfo_ptr->hello_rtn = func;

    return 0;
}

int net_register_handler(netinfo_type *netinfo_ptr, int usertype,
                         char *name, NETFP func)
{
    if (usertype < 0 || usertype > MAX_USER_TYPE)
        return -1;

    netinfo_ptr->userfuncs[usertype].func = func;
    netinfo_ptr->userfuncs[usertype].name = name;
    netinfo_ptr->userfuncs[usertype].totus = 0;
    netinfo_ptr->userfuncs[usertype].count = 0;

    return 0;
}

int is_real_netinfo(netinfo_type *netinfo_ptr)
{
    if (!netinfo_ptr->fake)
        return 1;
    else
        return 0;
}

int is_offload_netinfo(netinfo_type *netinfo_ptr)
{
    if (netinfo_ptr->offload)
        return 1;
    else
        return 0;
}


/* This function needs to be called with netinfo_ptr->lock held.
 *
 * Will get netinfo for node, check cache first:
 * if not the last used node then do a linear search in list
 */
static inline host_node_type *get_host_node_cache_ll(netinfo_type *netinfo_ptr,
                                                     const char *host)
{
    host_node_type *host_node_ptr = netinfo_ptr->last_used_node_ptr;
    if (!host_node_ptr || host_node_ptr->host != host) {
        host_node_ptr = get_host_node_by_name_ll(netinfo_ptr, host);
        netinfo_ptr->last_used_node_ptr = host_node_ptr;
        netinfo_ptr->last_used_node_miss_cntr++;
    } else {
        netinfo_ptr->last_used_node_hit_cntr++;
    }
    return host_node_ptr;
}


void net_inc_recv_cnt_from(netinfo_type *netinfo_ptr, char *host)
{
    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    host_node_type *host_node_ptr = get_host_node_cache_ll(netinfo_ptr, host);
    if (!host_node_ptr) {
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        logmsg(LOGMSG_ERROR, "%s: node not found %s\n", __func__, host);
        return;
    }

    ++host_node_ptr->udp_info.recv;
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

void net_reset_udp_stat(netinfo_type *netinfo_ptr)
{
    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (host_node_type *ptr = netinfo_ptr->head; ptr != NULL;
         ptr = ptr->next) {
        ptr->udp_info.sent = 0;
        ptr->udp_info.recv = 0;
    }

    netinfo_ptr->last_used_node_hit_cntr = 0;
    netinfo_ptr->last_used_node_miss_cntr = 0;
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

void print_all_udp_stat(netinfo_type *netinfo_ptr)
{
    if (!netinfo_ptr) return;
    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (host_node_type *ptr = netinfo_ptr->head; ptr != NULL;
         ptr = ptr->next) {
        struct sockaddr_in sin;
        sin.sin_addr = ptr->addr;
        sin.sin_family = AF_INET;
        sin.sin_port = htons(ptr->port);
        int port = ptr->port;
        uint64_t sent = ptr->udp_info.sent;
        char buf1[256];
#ifdef UDP_DEBUG
        uint64_t recv = ptr->udp_info.recv;
        printf("node:%s port:%5d recv:%7llu sent:%7lu %s\n", ptr->host, port,
               recv, sent, print_addr(&sin, buf1));
#else
        logmsg(LOGMSG_USER, "node:%s port:%5d sent:%7lu %s\n", ptr->host, port,
               sent, print_addr(&sin, buf1));
#endif
    }
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
    logmsg(LOGMSG_USER, "netinfo udp cache hits: %u misses: %u lastnode: %s\n",
           netinfo_ptr->last_used_node_hit_cntr,
           netinfo_ptr->last_used_node_miss_cntr,
           (netinfo_ptr->last_used_node_ptr
                ? netinfo_ptr->last_used_node_ptr->host
                : NULL));
}

void print_node_udp_stat(char *prefix, netinfo_type *netinfo_ptr,
                         const char *host)
{
    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    host_node_type *host_node_ptr = get_host_node_cache_ll(netinfo_ptr, host);
    if (!host_node_ptr) {
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        logmsg(LOGMSG_ERROR, "%s: node not found %s\n", __func__, host);
        return;
    }

    int port = host_node_ptr->port;
    uint64_t sent = host_node_ptr->udp_info.sent;
    uint64_t recv = host_node_ptr->udp_info.recv;
    struct in_addr addr = host_node_ptr->addr;
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    logmsg(LOGMSG_USER, "%snode:%s port:%5d recv:%7lu sent:%7lu [%s]\n", prefix,
           host, port, recv, sent, inet_ntoa(addr));
}

ssize_t net_udp_send(int udp_fd, netinfo_type *netinfo_ptr, const char *host,
                     size_t len, void *info)
{
    struct sockaddr_in paddr;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    host_node_type *host_node_ptr = get_host_node_cache_ll(netinfo_ptr, host);

    if (!host_node_ptr) {
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        extern const char *db_eid_invalid;
        if (strcmp(host, db_eid_invalid) == 0)
            return -999;
        logmsg(LOGMSG_ERROR, "%s: node not found %s\n", __func__, host);
        return -1;
    }
    ++host_node_ptr->udp_info.sent;
    paddr.sin_addr = host_node_ptr->addr;
    paddr.sin_port = htons(host_node_ptr->port);
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    paddr.sin_family = AF_INET;
    socklen_t addrlen = sizeof(paddr);

#ifdef UDP_DEBUG
    __atomic_add_fetch(&curr_udp_cnt, 1, __ATOMIC_SEQ_CST);
#endif

    net_delay(host);
    ssize_t nsent =
        sendto(udp_fd, info, len, 0, (struct sockaddr *)&paddr, addrlen);

    if (nsent < 0) {
        logmsgperror("net_udp_send:sendto");
        logmsg(LOGMSG_USER, "dest=%s, addr=%s\n", host,
               inet_ntoa(paddr.sin_addr));
    }

    return nsent;
}

static host_node_type *add_to_netinfo_ll(netinfo_type *netinfo_ptr,
                                         const char hostname[], int portnum)
{
    host_node_type *ptr;
    /* check to see if the node already exists */
    ptr = netinfo_ptr->head;
    if (debug_switch_offload_check_hostname() &&
        is_offload_netinfo(netinfo_ptr)) {
        while (ptr != NULL && (strcmp(ptr->host, hostname)))
            ptr = ptr->next;
    } else {
        while (ptr != NULL && ptr->host != hostname)
            ptr = ptr->next;
    }
    if (ptr != NULL) {
        return ptr;
    }

    ptr = calloc(1, sizeof(host_node_type));
    if (!ptr) {
        logmsg(LOGMSG_FATAL, "Can't allocate memory for netinfo\n");
        abort();
    }

#ifdef PER_THREAD_MALLOC
    if (gbl_verbose_net)
        logmsg(LOGMSG_INFO, "creating %d byte buffer pool for node %s\n",
               netinfo_ptr->pool_size, hostname);

    size_t scopelen = strlen(hostname) + sizeof(netinfo_ptr->service) + 2;
    char *scope = malloc(scopelen);
    if (scope == NULL) {
        logmsg(LOGMSG_ERROR, "%s: couldn't init msp for %s\n", __func__,
               hostname);
        free(ptr);
        return NULL;
    }

    snprintf(scope, scopelen, "%s@%s", netinfo_ptr->service, hostname);
    ptr->msp =
        comdb2ma_create_with_scope(netinfo_ptr->pool_size, 0, "NET", scope, 1);
    if (ptr->msp == NULL) {
        logmsg(LOGMSG_ERROR, "%s: couldn't init msp for %s\n", __func__,
               hostname);
        free(scope);
        free(ptr);
        return NULL;
    }
#endif /* PER_THREAD_MALLOC */

    Pthread_mutex_init(&(ptr->write_lock), NULL);

    ptr->netinfo_ptr = netinfo_ptr;
    ptr->closed = 1;
    ptr->really_closed = 1;
    ptr->fd = -1;

    ptr->next = netinfo_ptr->head;
    ptr->host = intern(hostname);
    ptr->hostname_len = strlen(ptr->host) + 1;
    /* ptr->addr will be set by connect_thread() */
    ptr->port = portnum;
    ptr->timestamp = time(NULL);

    Pthread_mutex_init(&(ptr->lock), NULL);
    Pthread_mutex_init(&(ptr->timestamp_lock), NULL);

    ptr->user_data_buf = malloc(netinfo_ptr->user_data_buf_size);

    Pthread_mutex_init(&(ptr->enquelk), NULL);
    Pthread_mutex_init(&(ptr->wait_mutex), NULL);
    Pthread_mutex_init(&(ptr->throttle_lock), NULL);
    Pthread_cond_init(&(ptr->ack_wakeup), NULL);
    Pthread_cond_init(&(ptr->write_wakeup), NULL);
    Pthread_cond_init(&(ptr->throttle_wakeup), NULL);

    if (netinfo_ptr->qstat_init_rtn) {
        ptr->qstat = (netinfo_ptr->qstat_init_rtn)(
            netinfo_ptr, netinfo_ptr->service, hostname);
    } else {
        ptr->qstat = NULL;
    }

    netinfo_ptr->head = ptr;

    char *metric_name = comdb2_asprintf("queue_size_%s", hostname);
    ptr->metric_queue_size = time_metric_new(metric_name);
    free(metric_name);

    return ptr;
}

host_node_type *add_to_netinfo(netinfo_type *netinfo_ptr, const char hostname[],
                               int portnum)
{
    host_node_type *ptr;

#ifdef DEBUGNET
    fprintf(stderr, "%s: adding %s\n", __func__, hostname);
#endif

    if (!isinterned(hostname))
        abort();

    /*override with smaller timeout*/
    portmux_set_default_timeout(100);

    /* don't add disallowed nodes */
    if (netinfo_ptr->allow_rtn &&
        !netinfo_ptr->allow_rtn(netinfo_ptr, hostname)) {
        logmsg(LOGMSG_ERROR, "%s: not allowed to add %s\n", __func__, hostname);
        return NULL;
    }

    /* we need to lock the netinfo to prevent creating too many connect threads
     */
    Pthread_rwlock_wrlock(&(netinfo_ptr->lock));

    ptr = add_to_netinfo_ll(netinfo_ptr, hostname, portnum);

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
    return ptr;
}

/* for debugging only */
void netinfo_lock(netinfo_type *netinfo_ptr, int seconds)
{
    logmsg(LOGMSG_USER, "grabbing exclusive access to netinfo lock\n");
    Pthread_rwlock_wrlock(&(netinfo_ptr->lock));
    logmsg(LOGMSG_USER, "sleeping for %d seconds\n", seconds);
    sleep(seconds);
    logmsg(LOGMSG_USER, "releasing netinfo lock\n");
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

static void rem_from_netinfo_ll(netinfo_type *netinfo_ptr,
                                host_node_type *host_node_ptr)
{
    host_node_type *tmp = netinfo_ptr->head;
    if (host_node_ptr == tmp) {
        netinfo_ptr->head = host_node_ptr->next;
    } else {
        while (tmp && tmp->next != host_node_ptr)
            tmp = tmp->next;
        if (tmp)
            tmp->next = host_node_ptr->next;
    }

    /* Call qstat free routine if its set */
    if (netinfo_ptr->qstat_free_rtn)
        (netinfo_ptr->qstat_free_rtn)(netinfo_ptr, host_node_ptr->qstat);

    // if last_used is eq to host_node_ptr->host, clear last_used_node_ptr
    if (host_node_ptr == netinfo_ptr->last_used_node_ptr) {
        netinfo_ptr->last_used_node_ptr = NULL;
    }

    if (host_node_ptr->write_head != NULL) {
        /* purge anything pending to be sent */
        Pthread_mutex_lock(&(host_node_ptr->write_lock));
        empty_write_list(host_node_ptr);
        Pthread_mutex_unlock(&(host_node_ptr->write_lock));
    }

    Pthread_mutex_lock(&(host_node_ptr->lock));
    close_hostnode_ll(host_node_ptr);
    while (host_node_ptr->have_connect_thread ||
           host_node_ptr->have_reader_thread ||
           host_node_ptr->have_writer_thread) {
        Pthread_mutex_unlock(&(host_node_ptr->lock));
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        sleep(1);
        Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
        Pthread_mutex_lock(&(host_node_ptr->lock));
        printf("%s:%s connect_thd:%d reader_thd:%d writer_thd:%d\n",
               netinfo_ptr->service, host_node_ptr->host,
               host_node_ptr->have_connect_thread,
               host_node_ptr->have_reader_thread,
               host_node_ptr->have_writer_thread);
    }
    Pthread_mutex_unlock(&(host_node_ptr->lock));
    Pthread_mutex_destroy(&(host_node_ptr->lock));
    Pthread_mutex_destroy(&(host_node_ptr->timestamp_lock));
    Pthread_mutex_destroy(&(host_node_ptr->write_lock));
    Pthread_mutex_destroy(&(host_node_ptr->enquelk));
    Pthread_mutex_destroy(&(host_node_ptr->wait_mutex));
    Pthread_mutex_destroy(&(host_node_ptr->throttle_lock));

    Pthread_cond_destroy(&(host_node_ptr->ack_wakeup));
    Pthread_cond_destroy(&(host_node_ptr->write_wakeup));
    Pthread_cond_destroy(&(host_node_ptr->throttle_wakeup));

#ifdef PER_THREAD_MALLOC
    comdb2ma_destroy(host_node_ptr->msp);
#endif /* PER_THREAD_MALLOC */

    free(host_node_ptr->user_data_buf);
    sbuf2free(host_node_ptr->sb);

#ifndef NDEBUG
    memset(host_node_ptr, 0, sizeof(host_node_type));
#endif
    free(host_node_ptr);
}

/* called from connect thread upon exiting:
 * when db is exiting or when host_node_ptr decom_flag is set
 */
static void rem_from_netinfo(netinfo_type *netinfo_ptr,
                             host_node_type *host_node_ptr)
{
    host_node_printf(LOGMSG_INFO, host_node_ptr, "rem_from_netinfo: node=%s\n",
                     host_node_ptr->host);

    if (!host_node_ptr)
        return;

    Pthread_rwlock_wrlock(&(netinfo_ptr->lock));
    rem_from_netinfo_ll(netinfo_ptr, host_node_ptr);
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

void net_cleanup_netinfo(netinfo_type *netinfo_ptr)
{
    host_node_type *ptr;
    Pthread_rwlock_wrlock(&(netinfo_ptr->lock));
    while ((ptr = netinfo_ptr->head) != NULL) {
        rem_from_netinfo_ll(netinfo_ptr, ptr);
    }
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

sanc_node_type *net_add_to_sanctioned(netinfo_type *netinfo_ptr,
                                      char hostname[], int portnum)
{
    sanc_node_type *ptr;

    /* don't add disallowed nodes */
    if (netinfo_ptr->allow_rtn &&
        !netinfo_ptr->allow_rtn(netinfo_ptr, hostname)) {
        logmsg(LOGMSG_ERROR, "net_add_to_sanctioned: not allowed to add %s\n",
                hostname);
        return NULL;
    }

    logmsg(LOGMSG_INFO, "net_add_to_sanctioned %s\n", hostname);

    Pthread_mutex_lock(&(netinfo_ptr->sanclk));

    ptr = add_to_sanctioned_nolock(netinfo_ptr, hostname, portnum);

    Pthread_mutex_unlock(&(netinfo_ptr->sanclk));

    return ptr;
}

int net_is_single_sanctioned_node(netinfo_type *netinfo_ptr)
{
    int single_node = 0 ;
    sanc_node_type *ptr;

    Pthread_mutex_lock(&(netinfo_ptr->sanclk));

    ptr = netinfo_ptr->sanctioned_list;

    if (ptr && !ptr->next && !strcmp(ptr->host, netinfo_ptr->myhostname))
        single_node = 1;

    Pthread_mutex_unlock(&(netinfo_ptr->sanclk));

    return single_node;
}

static int net_get_sanctioned_int(netinfo_type *netinfo_ptr, int max_nodes,
                                 const char *hosts[REPMAX], int include_self)
{
    int count = 0;
    sanc_node_type *ptr;

    Pthread_mutex_lock(&(netinfo_ptr->sanclk));

    for (ptr = netinfo_ptr->sanctioned_list; ptr != NULL; ptr = ptr->next) {
        if (ptr->host == netinfo_ptr->myhostname && !include_self)
            continue;

        if (count < max_nodes) {
            hosts[count] = ptr->host;
        }
        count++;
    }
    Pthread_mutex_unlock(&(netinfo_ptr->sanclk));

    return count;
}

int net_get_sanctioned_node_list(netinfo_type *netinfo_ptr, int max_nodes,
                                 const char *hosts[REPMAX])
{
    return net_get_sanctioned_int(netinfo_ptr, max_nodes, hosts, 1);
}

int net_get_sanctioned_replicants(netinfo_type *netinfo_ptr, int max_nodes,
                                 const char *hosts[REPMAX])
{
    return net_get_sanctioned_int(netinfo_ptr, max_nodes, hosts, 0);
}

int net_sanctioned_and_connected_nodes(netinfo_type *netinfo_ptr, int max_nodes,
                                       const char *hosts[REPMAX])
{
    host_node_type *ptr;
    sanc_node_type *ptr_sanc;
    int count = 0;
    int is_sanc = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        /* dont send to yourself */
        if (ptr->host == netinfo_ptr->myhostname)
            continue;

        /* dont count disconected guys */
        if (ptr->fd <= 0)
            continue;

        /* dont count guys that didnt hello us */
        if (!ptr->got_hello)
            continue;

        is_sanc = 0;
        Pthread_mutex_lock(&(netinfo_ptr->sanclk));
        for (ptr_sanc = netinfo_ptr->sanctioned_list; ptr_sanc != NULL;
             ptr_sanc = ptr_sanc->next) {
            if (strcmp(ptr_sanc->host, ptr->host) == 0
                /*&& ptr_sanc->port == ptr->port*/) {
                is_sanc = 1;
                break;
            }
        }
        Pthread_mutex_unlock(&(netinfo_ptr->sanclk));

        if (is_sanc) {
            hosts[count++] = ptr->host;
            if (count >= REPMAX)
                break;
        }
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return count;
}

/* This just pulls the given node out of the linked list and frees it.
 * We assume that the thread ids and pointers to other memory will all
 * be NULL and so we don't have to stop any threads/free other memory. */
int net_del_from_sanctioned(netinfo_type *netinfo_ptr, char *host)
{
    sanc_node_type *ptr, *last;

    Pthread_mutex_lock(&(netinfo_ptr->sanclk));

    ptr = netinfo_ptr->sanctioned_list;

    last = NULL;
    while (ptr != NULL && ptr->host != host) {
        last = ptr;
        ptr = ptr->next;
    }

    if (ptr != NULL) {
        if (last)
            last->next = ptr->next;
        else
            netinfo_ptr->sanctioned_list = ptr->next;
    }

    Pthread_mutex_unlock(&(netinfo_ptr->sanclk));

    if (ptr) {
        logmsg(LOGMSG_INFO, "net_del_from_sanctioned %s\n", host);
        myfree(ptr);
        return 0;
    } else {
        logmsg(LOGMSG_INFO, "net_del_from_sanctioned %s - not in sanc list\n",
                host);
        return -1;
    }
}

void net_set_portmux_register_interval(netinfo_type *netinfo_ptr, int x)
{
    if (x >= 0)
        netinfo_ptr->portmux_register_interval = x;
    else
        logmsg(LOGMSG_ERROR, 
               "net_set_portmux_register_interval: invalid argument, %d\n", x);
}

void net_set_throttle_percent(netinfo_type *netinfo_ptr, int x)
{
    if (x >= 0 && x <= 100)
        netinfo_ptr->throttle_percent = x;
    else
        logmsg(LOGMSG_ERROR, 
               "net_set_app_throttle_percent: invalid input, %d.\n",
               x);
}

void net_set_enque_reorder_lookahead(netinfo_type *netinfo_ptr, int x)
{
    netinfo_ptr->enque_reorder_lookahead = x;
}

void net_set_enque_flush_interval(netinfo_type *netinfo_ptr, int x)
{
    netinfo_ptr->enque_flush_interval = x;
}

int net_get_enque_flush_interval(netinfo_type *netinfo_ptr)
{
    return netinfo_ptr->enque_flush_interval;
}

void net_setbufsz(netinfo_type *netinfo_ptr, int bufsz)
{
    netinfo_ptr->bufsz = bufsz;
}

void net_exiting(netinfo_type *netinfo_ptr)
{
    netinfo_ptr->exiting = 1;
}

int net_is_exiting(netinfo_type *netinfo_ptr)
{
    return netinfo_ptr->exiting;
}

typedef struct netinfo_node {
    LINKC_T(struct netinfo_node) lnk;
    netinfo_type *netinfo_ptr;
} netinfo_node_t;
static LISTC_T(netinfo_node_t) nets_list;
static pthread_mutex_t nets_list_lk = PTHREAD_MUTEX_INITIALIZER;

netinfo_type *create_netinfo_int(char myhostname[], int myportnum, int myfd,
                                 char app[], char service[], char instance[],
                                 int fake, int offload, int ischild,
                                 int use_getservbyname)
{
    netinfo_type *netinfo_ptr;
    host_node_type *host_node_ptr;
    netinfo_node_t *netinfo_node;
    int rc;

    netinfo_ptr = calloc(1, sizeof(netinfo_type));
    if (!netinfo_ptr) {
        logmsg(LOGMSG_FATAL, "Can't allocate memory for netinfo entry\n");
        abort();
    }

    listc_init(&(netinfo_ptr->watchlist), offsetof(watchlist_node_type, lnk));

    /* default queue of 25000 */
    netinfo_ptr->max_queue = 25000;

    /* default queue of 512M */
    netinfo_ptr->max_bytes = 512 * 1024 /*k*/ * 1024 /*m*/;

    /* flush every 1000 sends */
    netinfo_ptr->enque_flush_interval = 1000;

    /* Only look 20 buffers ahead for reordering */
    netinfo_ptr->enque_reorder_lookahead = 20;

    netinfo_ptr->heartbeat_send_time = 5;
    netinfo_ptr->heartbeat_check_time = 10;

    netinfo_ptr->bufsz = 1 * 1024 * 1024;

    netinfo_ptr->accept_thread_created = 0;
    netinfo_ptr->portmux_register_time = 0;
    netinfo_ptr->portmux_register_interval = gbl_net_portmux_register_interval;
    netinfo_ptr->ischild = ischild;
    netinfo_ptr->use_getservbyname = use_getservbyname;

    if (myportnum > 0 && !ischild) {
        /* manually specified port in lrl */
        netinfo_ptr->port_from_lrl = 1;
    }

    if (myportnum <= 0 && !ischild && !fake) {
        if (netinfo_ptr->use_getservbyname) {
            myportnum = net_get_port_by_service(instance);
        }
        if (myportnum <= 0) {
            myportnum = portmux_register(app, service, instance);
            if (myportnum == -1) {
                logmsg(LOGMSG_FATAL, "couldnt register port\n");
                exit(1);
            }
            logmsg(LOGMSG_INFO, "i registered port %d for %s\n", myportnum,
                    service);
        } else {
            portmux_use(app, service, instance, myportnum);
        }
        netinfo_ptr->portmux_register_time = comdb2_time_epoch();
    }

    Pthread_attr_init(&(netinfo_ptr->pthread_attr_detach));

    rc = pthread_attr_setdetachstate(&(netinfo_ptr->pthread_attr_detach),
                                     PTHREAD_CREATE_DETACHED);
    if (rc != 0) {
        logmsg(LOGMSG_FATAL, "pthread_attr_setdetachstate failed\n");
        exit(1);
    }

#ifdef DEBUG
    Pthread_attr_setstacksize(&(netinfo_ptr->pthread_attr_detach), 1024 * 1024);
#else
    Pthread_attr_setstacksize(&(netinfo_ptr->pthread_attr_detach), 1024 * 256);
#endif

    Pthread_mutex_init(&(netinfo_ptr->connlk), NULL);

    netinfo_ptr->connpool =
        pool_setalloc_init(sizeof(connect_and_accept_t), 0, malloc, free);
    if (netinfo_ptr->connpool == NULL) {
        logmsg(LOGMSG_ERROR, "create_netinfo: couldn't init connect_and_accept pool\n");
        goto fail;
    }

    Pthread_rwlock_init(&(netinfo_ptr->lock), NULL);
    Pthread_mutex_init(&(netinfo_ptr->seqlock), NULL);
    Pthread_mutex_init(&(netinfo_ptr->watchlk), NULL);
    Pthread_mutex_init(&(netinfo_ptr->sanclk), NULL);

    netinfo_ptr->pool_size = 512 * 1024;
    netinfo_ptr->user_data_buf_size = 256 * 1024;

    netinfo_ptr->throttle_percent = 50;
    netinfo_ptr->seqnum = (getpid() * 65537);
    netinfo_ptr->myport = myportnum;
    netinfo_ptr->myhostname = intern(myhostname);
    netinfo_ptr->myhostname_len = strlen(netinfo_ptr->myhostname) + 1;

    memset(netinfo_ptr->userfuncs, 0, sizeof(netinfo_ptr->userfuncs));
    netinfo_ptr->fake = fake;
    netinfo_ptr->offload = offload;

    strncpy0(netinfo_ptr->app, app, sizeof(netinfo_ptr->app));
    strncpy0(netinfo_ptr->service, service, sizeof(netinfo_ptr->service));
    strncpy0(netinfo_ptr->instance, instance, sizeof(netinfo_ptr->instance));

    netinfo_ptr->stats.bytes_read = netinfo_ptr->stats.bytes_written = 0;
    netinfo_ptr->stats.throttle_waits = netinfo_ptr->stats.reorders = 0;

    host_node_ptr = add_to_netinfo(netinfo_ptr, myhostname, myportnum);
    if (host_node_ptr == NULL) {
        logmsg(LOGMSG_ERROR, "create_netinfo: couldn't add self to netinfo\n");
        goto fail;
    }
    netinfo_ptr->myfd = myfd;

    netinfo_node = malloc(sizeof(netinfo_node_t));
    if (netinfo_node == NULL) {
        logmsg(LOGMSG_ERROR, "create_netinfo: malloc failed. memstat on this "
                        "netinfo will not be tracked\n");
    } else {
        netinfo_node->netinfo_ptr = netinfo_ptr;
        Pthread_mutex_lock(&nets_list_lk);
        listc_atl(&nets_list, netinfo_node);
        Pthread_mutex_unlock(&nets_list_lk);
    }
    netinfo_ptr->hellofd = -1;

    netinfo_ptr->conntime_all = quantize_new(1, 100, "ms");
    netinfo_ptr->conntime_periodic = quantize_new(1, 100, "ms");
    netinfo_ptr->num_accepts = 0;
    netinfo_ptr->num_accept_timeouts = 0;
    netinfo_ptr->conntime_dump_period = 10 * 60;
    netinfo_ptr->num_current_non_appsock_accepts = 0;

    netinfo_ptr->num_accepts = 0;
    netinfo_ptr->num_accept_timeouts = 0;

    return netinfo_ptr;

fail:
    free(netinfo_ptr);
    return NULL;
}

inline netinfo_type *create_netinfo_fake(void)
{
    char myhostname[HOSTNAME_LEN] = "fakehost";
    int myportnum = -1;
    char app[HOSTNAME_LEN] = "fakeapp";
    char service[HOSTNAME_LEN] = "fakeservice";
    char instance[HOSTNAME_LEN] = "fakeinstance";

    return create_netinfo_int(intern(myhostname), myportnum, -1, app, service,
                              instance, 1, 0, 0, 0);
}

inline netinfo_type *create_netinfo(char myhostname[], int myportnum, int myfd,
                                    char app[], char service[], char instance[],
                                    int ischild, int use_getservbyname)
{
    return create_netinfo_int(myhostname, myportnum, myfd, app, service,
                              instance, 0, 0, ischild, use_getservbyname);
}

inline netinfo_type *create_netinfo_offload(char myhostname[], int myportnum,
                                            int myfd, char app[],
                                            char service[], char instance[])
{
    extern int gbl_accept_on_child_nets;
    return create_netinfo_int(myhostname, myportnum, myfd, app, service,
                              instance, 0, 1, !gbl_accept_on_child_nets, 0);
}

void net_count_nodes_ex(netinfo_type *netinfo_ptr, int *total_ptr,
                        int *connected_ptr)
{
    host_node_type *ptr;
    int total = 0, connected = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        total++;
        if (ptr->got_hello)
            connected++;
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    if (total_ptr)
        *total_ptr = total;
    if (connected_ptr)
        *connected_ptr = connected + 1; /* because I won't have had a hello
                                           from myself */
}

inline int net_count_nodes(netinfo_type *netinfo_ptr)
{
    if (!netinfo_ptr) return 0;
    int total;
    net_count_nodes_ex(netinfo_ptr, &total, NULL);
    return total;
}

inline int net_count_connected_nodes(netinfo_type *netinfo_ptr)
{
    int connected;
    net_count_nodes_ex(netinfo_ptr, NULL, &connected);
    return connected;
}

/* This appears to be unused -- Sam J 03/24/05 */
void print_netinfo(netinfo_type *netinfo_ptr)
{
    host_node_type *ptr;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        logmsg(LOGMSG_USER, "%s:%d fd=%d host=%s\n", ptr->host, ptr->port, ptr->fd,
                ptr->host);
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

/*
   caller is expected to pass in array of pointers and array of ints,
   specifying size of the array in numhosts.
   upon return, ports array will be filled with ints
   hosts array will be filled with pointers.  caller must free each
   pointer when done.  numhosts will be set upon return to indicate
   number of entries actually returned
 */
static int read_hostlist(netinfo_type *netinfo_ptr, SBUF2 *sb, char *hosts[],
                         int ports[], int *numhosts)
{
    int datasz;
    int i;
    int num = 0;
    char *data;
    int rc;
    int tmp;
    uint8_t *p_buf, *p_buf_end;

    rc = read_stream(netinfo_ptr, NULL, sb, &tmp, sizeof(int));
    if (rc < 0)
        return -1;
    if (rc != sizeof(int)) {
        return 1;
    }

    p_buf = (uint8_t *)&tmp;
    p_buf_end = ((uint8_t *)&tmp + sizeof(int));
    buf_get(&datasz, sizeof(int), p_buf, p_buf_end);

    /* some reasonable sanity check on datasz */
    if ((datasz < 10) || (datasz > 1024 * 1024)) {
        return 1;
    }

    data = malloc(datasz);
    p_buf = (uint8_t *)data;
    p_buf_end = ((uint8_t *)data + datasz);

    /* read one integer less than datasz because we already read
       datasz */
    rc = read_stream(netinfo_ptr, NULL, sb, data, datasz - sizeof(int));
    if (rc < 0)
        return -1;
    if (rc != (datasz - sizeof(int))) {
        free(data);
        return 1;
    }

    /* copy out the numhosts */
    p_buf = (uint8_t *)buf_get(&num, sizeof(num), p_buf, p_buf_end);

    /* make sure we only return what fits in the user's buffer */
    if (num < *numhosts)
        *numhosts = num;

    /* copy out the hosts, make sure the strings are \0 terminated */
    for (i = 0; i < *numhosts; i++) {
        hosts[i] = malloc(HOSTNAME_LEN + 1);
        p_buf =
            (uint8_t *)buf_no_net_get(hosts[i], HOSTNAME_LEN, p_buf, p_buf_end);
        hosts[i][HOSTNAME_LEN] = '\0';
    }

    /* copy out the ports */
    for (i = 0; i < *numhosts; i++) {
        int *p_port = (ports + i);
        p_buf = (uint8_t *)buf_get(p_port, sizeof(int), p_buf, p_buf_end);
        /* older comdb2 will not handle mangled ports and might hello us back
         * unmasked values */
        p_port[0] &= 0x0ffff;
    }

    /* read and discard node numbers */
    for (i = 0; i < *numhosts; i++) {
        int node;
        p_buf = (uint8_t *)buf_get(&node, sizeof(int), p_buf, p_buf_end);
    }

    for (i = 0; i < *numhosts; i++) {
        if (hosts[i][0] == '.') {
            int len = atoi(&hosts[i][1]);
            if (len > 4096) {
                for (int j = 0; j < *numhosts; j++) {
                    free(hosts[j]);
                }
                free(data);
                return 1;
            }
            hosts[i] = realloc(hosts[i], len);
            p_buf = (uint8_t *)buf_no_net_get(hosts[i], len, p_buf, p_buf_end);
        }
    }

    free(data);

    return 0;
}

static int read_user_data(host_node_type *host_node_ptr, int *type, int *seqnum,
                          int *needack, int *datalen, void **data,
                          int *malloced)
{
    int rc;
    net_send_message_header msghdr;
    uint8_t databf[NET_SEND_MESSAGE_HEADER_LEN], *p_buf, *p_buf_end;
    netinfo_type *netinfo_ptr = host_node_ptr->netinfo_ptr;
    SBUF2 *sb = host_node_ptr->sb;

    *malloced = 0;

    rc = read_stream(netinfo_ptr, host_node_ptr, sb, &databf, sizeof(databf));
    if (rc != sizeof(msghdr)) {
        host_node_errf(LOGMSG_ERROR, host_node_ptr,
                       "read_user_data:error reading user data header\n");
        goto fail;
    }

    p_buf = databf;
    p_buf_end = (databf + NET_SEND_MESSAGE_HEADER_LEN);

    net_send_message_header_get(&msghdr, p_buf, p_buf_end);

    *type = msghdr.usertype;
    *seqnum = msghdr.seqnum;
    *needack = msghdr.waitforack;
    *datalen = msghdr.datalen;

    if (*datalen > 0) {
        if (netinfo_ptr->trace && debug_switch_net_verbose())
            logmsg(LOGMSG_ERROR, "Reading %d bytes %llu\n", *datalen, gettmms());

        if (*datalen < (netinfo_ptr->user_data_buf_size)) {
            *data = host_node_ptr->user_data_buf;
            *malloced = 0;
        } else {
            *data = HOST_MALLOC(host_node_ptr, *datalen);
            *malloced = 1;
        }

        if (*data == NULL) {
            host_node_errf(LOGMSG_ERROR, host_node_ptr, "%s: malloc %d failed\n", __func__,
                           *datalen);
            goto fail;
        }
        rc = read_stream(netinfo_ptr, host_node_ptr, sb, *data, *datalen);
        if (rc != *datalen) {
            host_node_errf(LOGMSG_ERROR, host_node_ptr,
                           "read_user_data:error reading user_data, "
                           "wanted %d bytes, got %d\n",
                           *datalen, rc);

            if (*malloced)
                free(*data);

            goto fail;
        }
    } else {
        *data = NULL;
    }

    return 0;

fail:
    return -1;
}

int net_ack_message_payload(void *handle, int outrc, void *payload,
                            int payloadlen)
{
    uint8_t *ack_buf, *p_buf, *p_buf_end;
    net_ack_message_payload_type p_net_ack_payload_message;
    host_node_type *host_node_ptr;
    int rc = 0;
    ack_state_type *ack_state = handle;

    int sz = offsetof(net_ack_message_payload_type, payload) + payloadlen;

    if (ack_state->needack) {
        Pthread_rwlock_rdlock(&(ack_state->netinfo->lock));

        host_node_ptr =
            get_host_node_by_name_ll(ack_state->netinfo, ack_state->fromhost);

        if (host_node_ptr == NULL) {
            Pthread_rwlock_unlock(&(ack_state->netinfo->lock));
            return -1;
        }

        p_net_ack_payload_message.seqnum = ack_state->seqnum;
        p_net_ack_payload_message.outrc = outrc;
        p_net_ack_payload_message.paylen = payloadlen;

        ack_buf = alloca(sz);
        p_buf = ack_buf;
        p_buf_end = ack_buf + sz;

        p_buf = net_ack_message_payload_type_put(&p_net_ack_payload_message,
                                                 p_buf, p_buf_end);
        p_buf = buf_no_net_put(payload, payloadlen, p_buf, p_buf_end);

        /*fprintf(stderr, "net_ack_message: sending to %d\n",
          ack_state->from_node);*/

        rc = write_message(ack_state->netinfo, host_node_ptr,
                           WIRE_HEADER_ACK_PAYLOAD, ack_buf, sz);

        Pthread_rwlock_unlock(&(ack_state->netinfo->lock));
    }
    return rc;
}

static int process_payload_ack(netinfo_type *netinfo_ptr,
                               host_node_type *host_node_ptr)
{
    int rc;
    int seqnum, outrc;
    net_ack_message_payload_type p_net_ack_message_payload = {0};
    void *payload = NULL;
    uint8_t *buf, *p_buf, *p_buf_end;
    seq_data *ptr;

    buf = alloca(offsetof(net_ack_message_payload_type, payload));

    rc = read_stream(netinfo_ptr, host_node_ptr, host_node_ptr->sb, buf,
                     offsetof(net_ack_message_payload_type, payload));
    if (rc != offsetof(net_ack_message_payload_type, payload))
        return -1;

    p_buf = buf;
    p_buf_end = buf + offsetof(net_ack_message_payload_type, payload);

    net_ack_message_payload_type_get(&p_net_ack_message_payload, p_buf,
                                     p_buf_end);

    seqnum = p_net_ack_message_payload.seqnum;
    outrc = p_net_ack_message_payload.outrc;

    if (p_net_ack_message_payload.paylen > 1024 ||
        p_net_ack_message_payload.paylen <= 0)
        return -1;

    payload = HOST_MALLOC(host_node_ptr, p_net_ack_message_payload.paylen);
    rc = read_stream(netinfo_ptr, host_node_ptr, host_node_ptr->sb, payload,
                     p_net_ack_message_payload.paylen);

    Pthread_mutex_lock(&(host_node_ptr->wait_mutex));

    ptr = host_node_ptr->wait_list;

    while (ptr != NULL && (ptr->seqnum != seqnum)) {
        ptr = ptr->next;
    }

    if (ptr == NULL) {
        free(payload);
    } else {
        ptr->outrc = outrc;
        ptr->payload = payload;
        ptr->payloadlen = p_net_ack_message_payload.paylen;
        ptr->ack = 1;
        Pthread_cond_broadcast(&(host_node_ptr->ack_wakeup));
    }

    Pthread_mutex_unlock(&(host_node_ptr->wait_mutex));

    return 0;
}

static int process_ack(netinfo_type *netinfo_ptr, host_node_type *host_node_ptr)
{
    int rc;
    int seqnum, outrc;
    net_ack_message_type p_net_ack_message;
    uint8_t buf[NET_ACK_MESSAGE_TYPE_LEN], *p_buf, *p_buf_end;
    seq_data *ptr;

    rc = read_stream(netinfo_ptr, host_node_ptr, host_node_ptr->sb, buf,
                     NET_ACK_MESSAGE_TYPE_LEN);
    if (rc != NET_ACK_MESSAGE_TYPE_LEN)
        return -1;

    p_buf = buf;
    p_buf_end = buf + NET_ACK_MESSAGE_TYPE_LEN;

    net_ack_message_type_get(&p_net_ack_message, p_buf, p_buf_end);

    seqnum = p_net_ack_message.seqnum;
    outrc = p_net_ack_message.outrc;

    Pthread_mutex_lock(&(host_node_ptr->wait_mutex));

    ptr = host_node_ptr->wait_list;

    while (ptr != NULL && (ptr->seqnum != seqnum)) {
        ptr = ptr->next;
    }

    if (ptr != NULL) {
        ptr->outrc = outrc;
        ptr->ack = 1;
        Pthread_cond_broadcast(&(host_node_ptr->ack_wakeup));
    }

    Pthread_mutex_unlock(&(host_node_ptr->wait_mutex));

    return 0;
}

int net_ack_message(void *handle, int outrc)
{
    uint8_t ack_buf[NET_ACK_MESSAGE_TYPE_LEN], *p_buf, *p_buf_end;
    net_ack_message_type p_net_ack_message;
    host_node_type *host_node_ptr;
    int rc = 0;
    ack_state_type *ack_state = handle;

    if (ack_state->needack) {
        Pthread_rwlock_rdlock(&(ack_state->netinfo->lock));

        host_node_ptr =
            get_host_node_by_name_ll(ack_state->netinfo, ack_state->fromhost);

        if (host_node_ptr == NULL) {
            Pthread_rwlock_unlock(&(ack_state->netinfo->lock));
            return -1;
        }

        p_net_ack_message.seqnum = ack_state->seqnum;
        p_net_ack_message.outrc = outrc;

        p_buf = ack_buf;
        p_buf_end = ack_buf + NET_ACK_MESSAGE_TYPE_LEN;

        net_ack_message_type_put(&p_net_ack_message, p_buf, p_buf_end);

        /*fprintf(stderr, "net_ack_message: sending to %d\n",
          ack_state->from_node);*/

        rc = write_message(ack_state->netinfo, host_node_ptr, WIRE_HEADER_ACK,
                           ack_buf, sizeof(ack_buf));

        Pthread_rwlock_unlock(&(ack_state->netinfo->lock));
    }
    return rc;
}

static int process_user_message(netinfo_type *netinfo_ptr,
                                host_node_type *host_node_ptr)
{
    int usertype, seqnum, datalen, needack;
    ack_state_type *ack_state = NULL;
    void *data;

    /* deliver nothing for fake netinfo */
    if (netinfo_ptr->fake || netinfo_ptr->exiting)
        return 0;


    int malloced = 0;

    int rc = read_user_data(host_node_ptr, &usertype, &seqnum, &needack,
                            &datalen, &data, &malloced);

#if 0
    logmsg(LOGMSG_DEBUG, "process_user_message from %s, ut=%d\n",
           host_node_ptr->host, usertype);
#endif

    if (rc != 0)
        return -1; /* not sure ... exit the reader thread??? */

    if (usertype >= 0 && usertype <= MAX_USER_TYPE &&
         netinfo_ptr->userfuncs[usertype].func != NULL) {
        if (needack) {
            ack_state = HOST_MALLOC(host_node_ptr, sizeof(ack_state_type));
            ack_state->seqnum = seqnum;
            ack_state->needack = needack;
            ack_state->fromhost = host_node_ptr->host;
            ack_state->netinfo = netinfo_ptr;
        } else {
            ack_state = NULL;
        }

        Pthread_mutex_lock(&(host_node_ptr->timestamp_lock));
        host_node_ptr->running_user_func = 1;
        Pthread_mutex_unlock(&(host_node_ptr->timestamp_lock));

        int64_t start_us = comdb2_time_epochus();
        /* run the user's function */
        netinfo_ptr->userfuncs[usertype].func(
            ack_state, netinfo_ptr->usrptr, host_node_ptr->host, usertype,
            data, datalen, 1);
        netinfo_ptr->userfuncs[usertype].count++;
        netinfo_ptr->userfuncs[usertype].totus +=
            (comdb2_time_epochus() - start_us);

        /* update timestamp before checking it */
        Pthread_mutex_lock(&(host_node_ptr->timestamp_lock));
        host_node_ptr->timestamp = time(NULL);
        host_node_ptr->running_user_func = 0;
        Pthread_mutex_unlock(&(host_node_ptr->timestamp_lock));
    } else {
        static int lastpr = 0, count = 0;
        int now;
        count++;
        if ((now = comdb2_time_epoch()) - lastpr) {
            host_node_printf(LOGMSG_INFO, host_node_ptr,
                             "%s: unexpected usertype:%d, count=%d\n", __func__,
                             usertype, count);
            lastpr = now;
        }
    }

    if (ack_state)
        free(ack_state);

    if (malloced)
        free(data);

    return 0;
}

/* remove node from the netinfo list */
void net_decom_node(netinfo_type *netinfo_ptr, const char *host)
{
    if (host && netinfo_ptr->myhostname == host) {
        return;
    }

    Pthread_rwlock_wrlock(&(netinfo_ptr->lock));

    /* remove the host node from the netinfo list */
    host_node_type *host_ptr, *host_back;
    host_back = host_ptr = netinfo_ptr->head;
    while (host_ptr != NULL && host_ptr->host != host) {
        host_back = host_ptr;
        host_ptr = host_ptr->next;
    }
    if (host_ptr != NULL) {
        if (host_ptr == netinfo_ptr->head)
            netinfo_ptr->head = host_ptr->next;
        else
            host_back->next = host_ptr->next;

        host_ptr->decom_flag = 1;

        if (host_ptr == netinfo_ptr->last_used_node_ptr)
            netinfo_ptr->last_used_node_ptr = NULL; // clear last_used_node_ptr
    }

    /* we can't free the host node pointer memory -
       let the connect thread do that */
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

struct net_decom_node_arg {
    netinfo_type *netinfo_ptr;
    char *host;
};

/* run net_decom_node after sleeping for a few sec
 */
static void *net_decom_node_delayed(void *p)
{
    struct net_decom_node_arg *args = (struct net_decom_node_arg *)p;
    sleep(2);
    net_decom_node(args->netinfo_ptr, args->host);
    free(args);
    return NULL;
}

static int run_net_decom_node_delayed(netinfo_type *netinfo_ptr,
                                      const char *host)
{
    /* dangerous to run this later as things are getting shutdown */
    if (gbl_exit) {
        return 0;
    }
    pthread_t tid;
    struct net_decom_node_arg *args;
    args = malloc(sizeof(struct net_decom_node_arg));
    if (args == NULL) {
        errno = ENOMEM;
        return -1;
    }

    args->host = host ? intern(host) : NULL;
    args->netinfo_ptr = netinfo_ptr;
    int rc = pthread_create(&tid, &(netinfo_ptr->pthread_attr_detach),
                            net_decom_node_delayed, args);
    if (rc != 0) {
       logmsg(LOGMSG_ERROR, "%s: pthread_create reader_thread failed rc %d\n", __func__, rc);
    }

    return 0;
}

int write_decom_msg(struct netinfo_struct *n, struct host_node_tag *h, int type,
                    void *a, int alen, void *b, int blen)
{
    struct iovec iov[] = {{.iov_base = a, .iov_len = alen},
                          {.iov_base = b, .iov_len = blen}};
    return write_message_int(n, h, type, iov, b ? 2 : 1,
                             WRITE_MSG_NOLIMIT | WRITE_MSG_NOHELLOCHECK |
                                 WRITE_MSG_NODELAY | WRITE_MSG_HEAD);
}

static int write_decom_hostname(netinfo_type *netinfo_ptr,
                                host_node_type *host_node_ptr,
                                const char *decom_host, int decom_hostlen,
                                const char *to_host)
{
    int a = htonl(decom_hostlen);
    return write_decom_msg(netinfo_ptr, host_node_ptr, WIRE_HEADER_DECOM_NAME,
                           &a, sizeof(a), (void *)decom_host, decom_hostlen);
}

static decom_writer *write_decom_impl = write_decom_hostname;
void set_decom_writer(decom_writer *impl)
{
    write_decom_impl = impl;
}

/* write decom message to to_host */
static int write_decom(netinfo_type *netinfo_ptr, host_node_type *host_node_ptr,
                       const char *decom_host, int decom_hostlen,
                       const char *to_host)
{
    return write_decom_impl(netinfo_ptr, host_node_ptr, decom_host,
                            decom_hostlen, to_host);
}


/* send a decom message about node "decom_host" to node "to_host" */
static int net_send_decom(netinfo_type *netinfo_ptr, const char *decom_host,
                          const char *to_host)
{
    host_node_type *host_node_ptr;
#ifdef DEBUG
    fprintf(stderr, "net_send_decom [%s] to_node=%d decom_node=%d\n",
            netinfo_ptr->service, to_host, decom_host);
#endif

    /* grab the host-node ptr for the to_host */
    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    host_node_ptr = netinfo_ptr->head;
    while (host_node_ptr != NULL && host_node_ptr->host != to_host)
        host_node_ptr = host_node_ptr->next;

    if (host_node_ptr == NULL) {
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
#ifdef DEBUG
        fprintf(stderr, "net_send_decom [%s] not found %s\n",
                netinfo_ptr->service, to_host);
#endif
        return -1;
    }

    int decom_hostlen = strlen(decom_host) + 1;
    int rc = write_decom(netinfo_ptr, host_node_ptr, decom_host, 
                         decom_hostlen, to_host);
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return rc;
}

static int process_decom_int(netinfo_type *netinfo_ptr, char *host)
{
    net_decom_node(netinfo_ptr, host);
    run_net_decom_node_delayed(netinfo_ptr, host);
    return 0;
}

int net_send_decom_all(netinfo_type *netinfo_ptr, char *decom_host)
{
    int outrc = 0;
    const char *nodes[REPMAX];
    int count = net_get_all_nodes(netinfo_ptr, nodes);
    for (int i = 0; i < count; i++) {
        logmsg(LOGMSG_INFO, "%s: [%s] decom:%s to:%s\n", __func__,
               netinfo_ptr->service, decom_host, nodes[i]);
        int rc = net_send_decom(netinfo_ptr, decom_host, nodes[i]);
        if (rc != 0) {
            outrc++;
            logmsg(LOGMSG_ERROR, "rc=%d sending decom to node %s\n", rc,
                   nodes[i]);
        }
    }
    process_decom_int(netinfo_ptr, decom_host);
    return outrc;
}


int net_is_connected(netinfo_type *netinfo_ptr, const char *host)
{
    host_node_type *host_node_ptr;
    int rc = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    host_node_ptr = netinfo_ptr->head;

    while (host_node_ptr != NULL && host_node_ptr->host != host)
        host_node_ptr = host_node_ptr->next;

    if (host_node_ptr && (host_node_ptr->fd != -1))
        rc = 1;

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return rc;
}


int net_send_hello(netinfo_type *netinfo_ptr, const char *tohost)
{
    host_node_type *host_node_ptr;
    int rc = -1;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    host_node_ptr = netinfo_ptr->head;
    while (host_node_ptr != NULL && host_node_ptr->host != tohost)
        host_node_ptr = host_node_ptr->next;
    if (host_node_ptr)
        rc = write_hello(netinfo_ptr, host_node_ptr);
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return rc;
}

static int process_decom(netinfo_type *net, host_node_type *host)
{
    int node, rc;
    rc = read_stream(net, host, host->sb, &node, sizeof(int));
    if (rc != sizeof(int)) {
        logmsg(LOGMSG_ERROR,
               "%s:err from read_stream attempting to read host, rc=%d",
               __func__, rc);
        return -1;
    }
    node = ntohl(node);
    return process_decom_int(net, hostname(node));
}

static int process_decom_name(netinfo_type *netinfo_ptr,
                              host_node_type *host_node_ptr)
{
    int hostlen;
    char *host, *ihost;
    int rc;
    rc = read_stream(netinfo_ptr, host_node_ptr, host_node_ptr->sb, &hostlen,
                     sizeof(int));
    if (rc != sizeof(int)) {
        logmsg(LOGMSG_ERROR,
               "%s:err from read_stream attempting to read host length, rc=%d",
               __func__, rc);
        return -1;
    }
    hostlen = ntohl(hostlen);
    if (hostlen > 256) {
        logmsg(LOGMSG_ERROR, "%s:absurd length for hostname, %d\n", __func__,
               hostlen);
        return -1;
    }
    host = HOST_MALLOC(host_node_ptr, hostlen);
    if (host == NULL) {
        logmsg(LOGMSG_ERROR, "%s:err can't allocate %d bytes for hostname\n",
               __func__, hostlen);
        return -1;
    }
    rc = read_stream(netinfo_ptr, host_node_ptr, host_node_ptr->sb, host,
                     hostlen);
    if (rc != hostlen) {
        logmsg(LOGMSG_ERROR,
               "%s:err from read_stream attempting to read host, rc=%d",
               __func__, rc);
        free(host);
        return -1;
    }
    ihost = intern(host);
    free(host);
    return process_decom_int(netinfo_ptr, ihost);
}


/* This must be called while holding the host_node_ptr->lock.
 * host_node_ptr->fd and host_node_ptr->sb should have been set up
 * to valid values before calling this.
 */
static int create_reader_writer_threads(host_node_type *host_node_ptr,
                                        const char *funcname)
{
    int rc;
    if (host_node_ptr->netinfo_ptr->exiting)
        return 0;

    /* make sure we have a reader thread */
    if (!(host_node_ptr->have_reader_thread)) {
        rc = pthread_create(&(host_node_ptr->reader_thread_id),
                            &(host_node_ptr->netinfo_ptr->pthread_attr_detach),
                            reader_thread, host_node_ptr);
        if (rc != 0) {
            host_node_errf(LOGMSG_ERROR, host_node_ptr,
                           "%s: pthread_create reader_thread failed: %d %s\n",
                           funcname, rc, strerror(rc));
            return -1;
        } else {
            host_node_ptr->have_reader_thread = 1;
        }
    }

    /* make sure we have a writer thread */
    if (!(host_node_ptr->have_writer_thread)) {
        rc = pthread_create(&(host_node_ptr->writer_thread_id),
                            &(host_node_ptr->netinfo_ptr->pthread_attr_detach),
                            writer_thread, host_node_ptr);
        if (rc != 0) {
            host_node_errf(LOGMSG_ERROR, host_node_ptr,
                           "%s: pthread_create writer_thread failed: %d %s\n",
                           funcname, rc, strerror(rc));
            return -1;
        } else {
            host_node_ptr->have_writer_thread = 1;
        }
    }

    return 0;
}

void kill_subnet(const char *subnet)
{
    host_node_type *ptr;
    int len = strlen(subnet) + 1;
    netinfo_node_t *curpos, *tmppos;

    Pthread_mutex_lock(&nets_list_lk);
    LISTC_FOR_EACH_SAFE(&nets_list, curpos, tmppos, lnk)
    {
        netinfo_type *netinfo_ptr = curpos->netinfo_ptr;
        ptr = netinfo_ptr->head;

        while (ptr != NULL) {
            if (!strncmp(ptr->subnet, subnet, len)) {
                if (!ptr->closed) {
                    logmsg(LOGMSG_INFO, "Shutting down socket for %s %s\n",
                           ptr->host, ptr->netinfo_ptr->service);
                    shutdown_hostnode_socket(ptr);
                } else {
                    logmsg(LOGMSG_INFO, "Already closed socket for %s %s\n",
                           ptr->host, ptr->netinfo_ptr->service);
                }
            }
            ptr = ptr->next;
        }
    }
    Pthread_mutex_unlock(&nets_list_lk);
}

int gbl_net_writer_thread_poll_ms = 1000;

static void *writer_thread(void *args)
{
    netinfo_type *netinfo_ptr;
    host_node_type *host_node_ptr;
    write_data *write_list_ptr, *write_list_back;
    int rc, flags, maxage;
    struct timespec waittime;
#ifndef HAS_CLOCK_GETTIME
    struct timeval tv;
#endif
    thread_started("net writer");
    THREAD_TYPE(__func__);

    host_node_ptr = args;
    netinfo_ptr = host_node_ptr->netinfo_ptr;

    host_node_ptr->writer_thread_arch_tid = getarchtid();
    if (gbl_verbose_net)
        host_node_printf(LOGMSG_DEBUG, host_node_ptr, "%s: starting tid=%d\n", __func__,
                         host_node_ptr->writer_thread_arch_tid);

    if (netinfo_ptr->start_thread_callback)
        netinfo_ptr->start_thread_callback(netinfo_ptr->callback_data);

    rc = write_hello(netinfo_ptr, host_node_ptr);

    Pthread_mutex_lock(&(host_node_ptr->enquelk));

    while (!host_node_ptr->decom_flag && !host_node_ptr->closed &&
           !netinfo_ptr->exiting) {
        while (host_node_ptr->write_head != NULL) {
            unsigned count, bytes;
            int start_time, end_time, diff_time;

            /* grab the entire list and reset enqueue counters */
            write_list_back = write_list_ptr = host_node_ptr->write_head;
            host_node_ptr->write_head = host_node_ptr->write_tail = NULL;
            count = host_node_ptr->enque_count;
            bytes = host_node_ptr->enque_bytes;
            host_node_ptr->enque_count = 0;
            host_node_ptr->enque_bytes = 0;

            if (netinfo_ptr->qstat_clear_rtn) {
                (netinfo_ptr->qstat_clear_rtn)(netinfo_ptr,
                                               host_node_ptr->qstat);
            }

            /* release this before writing to sock*/
            Pthread_mutex_unlock(&(host_node_ptr->enquelk));

            Pthread_cond_broadcast(&(host_node_ptr->throttle_wakeup));

            rc = 0;
            flags = 0;
            maxage = 0;

            Pthread_mutex_lock(&(host_node_ptr->write_lock));
            start_time = comdb2_time_epoch();
            while (write_list_ptr != NULL) {
                /* stop writing if we've hit an error or if we've disconnected
                 */
                if (!host_node_ptr->closed && rc >= 0) {
                    int age;
                    wire_header_type *wire_header, tmp_wire_hdr;
                    uint8_t *p_buf, *p_buf_end;

                    if (flags & WRITE_MSG_NODELAY) {
                        age = comdb2_time_epoch() - write_list_ptr->enque_time;
                        if (age > maxage)
                            maxage = age;
                    }

                    /* File in the wire header with correct details for our
                     * current connection. */

                    wire_header = &write_list_ptr->payload.header;
                    if (netinfo_ptr->myhostname_len > HOSTNAME_LEN) {
                        snprintf(tmp_wire_hdr.fromhost,
                                 sizeof(tmp_wire_hdr.fromhost), ".%d",
                                 netinfo_ptr->myhostname_len);
                    } else {
                        strncpy0(tmp_wire_hdr.fromhost, netinfo_ptr->myhostname,
                                 sizeof(tmp_wire_hdr.fromhost));
                    }
                    tmp_wire_hdr.fromport = netinfo_ptr->myport;
                    tmp_wire_hdr.fromnode = 0;
                    if (host_node_ptr->hostname_len > HOSTNAME_LEN) {
                        snprintf(tmp_wire_hdr.tohost,
                                 sizeof(tmp_wire_hdr.tohost), ".%d",
                                 host_node_ptr->hostname_len);
                    } else {
                        strncpy0(tmp_wire_hdr.tohost, host_node_ptr->host,
                                 sizeof(tmp_wire_hdr.tohost));
                    }
                    tmp_wire_hdr.toport = host_node_ptr->port;
                    tmp_wire_hdr.tonode = 0;
                    tmp_wire_hdr.type = wire_header->type;

                    /* This shouldn't happen.. but for a while it was happening
                     * due to various races. */
                    if (tmp_wire_hdr.toport == 0)
                        host_node_errf(LOGMSG_WARN, host_node_ptr, "PORT IS ZERO! type %d\n",
                                       tmp_wire_hdr.type);

                    p_buf = (uint8_t *)wire_header;
                    p_buf_end = ((uint8_t *)wire_header + sizeof(*wire_header));

                    /* endianize this */
                    net_wire_header_put(&tmp_wire_hdr, p_buf, p_buf_end);

                    rc = write_stream(
                        netinfo_ptr, host_node_ptr, host_node_ptr->sb,
                        write_list_ptr->payload.raw, write_list_ptr->len);
                    flags |= write_list_ptr->flags;
                } else
                    rc = -1;

                write_list_back = write_list_ptr;
                write_list_ptr = write_list_ptr->next;
                free(write_list_back);
            }
            /* we seem to set nodelay on virtually every message.  try to get
             * slightly better streaming performance by moving the flush out of
             * the main loop. */
            if (flags & WRITE_MSG_NODELAY) {
                net_delay(host_node_ptr->host);
                if (netinfo_ptr->trace && debug_switch_net_verbose())
                    logmsg(LOGMSG_USER, "Flushing %llu\n", gettmms());
                sbuf2flush(host_node_ptr->sb);
            }
            end_time = comdb2_time_epoch();
            Pthread_mutex_unlock(&(host_node_ptr->write_lock));

            diff_time = end_time - start_time;
            if (diff_time >= 2) {
                /* this is really informational now so I won't use
                 * capitals.  this trace dosn't necessarily mean that the
                 * network
                 * is being unreasonable. */
                host_node_errf(LOGMSG_WARN, host_node_ptr,
                               "%s: long write %d secs %u items %u bytes\n",
                               __func__, diff_time, count, bytes);
            }

            Pthread_mutex_lock(&(host_node_ptr->enquelk));
            if (rc < 0) {
                goto done;
            }
        }

#ifdef HAS_CLOCK_GETTIME
        rc = clock_gettime(CLOCK_REALTIME, &waittime);
#else
        rc = gettimeofday(&tv, NULL);
        timeval_to_timespec(&tv, &waittime);
#endif
        add_millisecs_to_timespec(&waittime, gbl_net_writer_thread_poll_ms);

        pthread_cond_timedwait(&(host_node_ptr->write_wakeup),
                               &(host_node_ptr->enquelk), &waittime);

        /*
           Pthread_cond_wait(&(host_node_ptr->write_wakeup),
           &(host_node_ptr->enquelk));
         */

        /*fprintf(stderr, "writer_thread: past pthread_cond_timedwait\n");*/
    }

done:
    Pthread_mutex_unlock(&(host_node_ptr->enquelk));

    Pthread_mutex_lock(&(host_node_ptr->lock));
    host_node_ptr->have_writer_thread = 0;
    if (gbl_verbose_net)
        host_node_printf(LOGMSG_DEBUG, host_node_ptr, "%s exiting\n", __func__);
    close_hostnode_ll(host_node_ptr);
    Pthread_mutex_unlock(&(host_node_ptr->lock));

    if (netinfo_ptr->stop_thread_callback)
        netinfo_ptr->stop_thread_callback(netinfo_ptr->callback_data);

    return NULL;
}


static int process_hello(netinfo_type *netinfo_ptr,
                         host_node_type *host_node_ptr)
{
    int rc = process_hello_common(netinfo_ptr, host_node_ptr, 1);
    if (rc == 1) {
        logmsg(LOGMSG_ERROR, "rejected hello from %s\n", host_node_ptr->host);
    }
    if (rc == 0) {
        write_hello_reply(netinfo_ptr, host_node_ptr);
    } else if (rc != -1) {
        /* Only propogate IO errors backwards. */
        rc = 0;
    }
    return rc;
}


static int process_hello_reply(netinfo_type *netinfo_ptr,
                               host_node_type *host_node_ptr)
{
    int rc = process_hello_common(netinfo_ptr, host_node_ptr, 0);
    if (rc == 1) {
        logmsg(LOGMSG_ERROR, "rejected hello reply from %s\n", host_node_ptr->host);
    }
    if (rc != -1) {
        /* Only propogate IO errors backwards. */
        rc = 0;
    }
    return rc;
}

/* Common code for processing hello and hello reply.
 *
 * Inputs: look_for_magic==1 if we want to check for the MAGICNODE.
 *
 * Returns:
 *    1     Hello was rejected (contained bad port numbers or error in
 *          read_hostlist).
 *    -1    IO error, reader thread cleans up.
 *    0     Success.
 */
static int process_hello_common(netinfo_type *netinfo_ptr,
                                host_node_type *host_node_ptr,
                                int look_for_magic)
{
    char *hosts[REPMAX];
    int ports[REPMAX];
    host_node_type *newhost, *fndhost;
    int rc;
    int numhosts = REPMAX;

    rc = read_hostlist(netinfo_ptr, host_node_ptr->sb, hosts, ports, &numhosts);
    if (rc < 0)
        return -1; /* reader thread cleans up */
    if (rc != 0) {
        logmsg(LOGMSG_ERROR, 
               "process_hello_common:error from read_hostlist, rc=%d\n", rc);
        return 0;
    }

    /* add each host, dont worry, dupes wont be added */
    for (int i = 0; i < numhosts; i++) {
        // only truly new hosts will return non-NULL
        newhost = add_to_netinfo(netinfo_ptr, intern(hosts[i]), ports[i]);

        if (newhost != NULL) {
            /* get readlock to prevent host from disappearing */
            Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

            fndhost = get_host_node_by_name_ll(netinfo_ptr, intern(hosts[i]));
            if (fndhost)
                connect_to_host(netinfo_ptr, fndhost, host_node_ptr);

            Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        }

        host_node_ptr->got_hello = 1;
    }

    for (int i = 0; i < numhosts; i++)
        free(hosts[i]);

    return 0;
}

static void *reader_thread(void *arg)
{
    netinfo_type *netinfo_ptr;
    host_node_type *host_node_ptr;
    wire_header_type wire_header;
    int rc, set_qstat = 0;
    char fromhost[256], tohost[256];

    thread_started("net reader");
    THREAD_TYPE(__func__);

    host_node_ptr = arg;
    netinfo_ptr = host_node_ptr->netinfo_ptr;

    host_node_ptr->reader_thread_arch_tid = getarchtid();
    if (gbl_verbose_net)
        host_node_printf(LOGMSG_DEBUG, host_node_ptr, "%s: starting tid=%d\n", __func__,
                         host_node_ptr->reader_thread_arch_tid);

    if (netinfo_ptr->start_thread_callback)
        netinfo_ptr->start_thread_callback(netinfo_ptr->callback_data);

    while (!host_node_ptr->decom_flag && !host_node_ptr->closed &&
           !netinfo_ptr->exiting) {

        if (set_qstat == 0 && netinfo_ptr->qstat_reader_rtn) {
            (netinfo_ptr->qstat_reader_rtn)(netinfo_ptr, host_node_ptr->qstat);
            set_qstat = 1;
        }

        host_node_ptr->timestamp = time(NULL);

        if (netinfo_ptr->trace && debug_switch_net_verbose())
           logmsg(LOGMSG_USER, "RT: reading header %llu\n", gettmms());

        rc = read_message_header(netinfo_ptr, host_node_ptr, &wire_header,
                                 fromhost, tohost);
        if (rc != 0) {
            if (!host_node_ptr->distress) {
                host_node_printf(LOGMSG_WARN, host_node_ptr, "entering distress mode\n");
            }
            /* if we loop it should be ok; TODO: maybe wanna have
             * a modulo operation to report errors w/ a certain periodicity? */
            host_node_ptr->distress++;
            break;
        } else {
            if (host_node_ptr->distress) {
                unsigned cycles = host_node_ptr->distress;
                host_node_ptr->distress = 0;
                host_node_printf(LOGMSG_INFO, host_node_ptr,
                                 "%s: leaving distress mode after %u cycles\n",
                                 __func__, cycles);
            }
        }

        /* We received data - update our timestamp.  We used to do this only
         * for heartbeat messages; do this for all types of message. */
        host_node_ptr->timestamp = comdb2_time_epoch();

        if (netinfo_ptr->trace && debug_switch_net_verbose())
           logmsg(LOGMSG_USER, "RT: got packet type=%d %llu\n", wire_header.type,
                   gettmms());

        switch (wire_header.type) {
        case WIRE_HEADER_HEARTBEAT:
            /* No special processing for heartbeats */
            break;

        case WIRE_HEADER_HELLO:
            rc = process_hello(netinfo_ptr, host_node_ptr);
            if (rc != 0) {
                logmsg(LOGMSG_ERROR, "reader thread: hello error from host %s\n",
                        host_node_ptr->host);
                goto done;
            }
            break;

        case WIRE_HEADER_HELLO_REPLY:
            rc = process_hello_reply(netinfo_ptr, host_node_ptr);
            if (rc != 0) {
                logmsg(LOGMSG_ERROR, "reader thread: hello error from host %s\n",
                        host_node_ptr->host);
                goto done;
            }
            break;

        case WIRE_HEADER_DECOM:
            rc = process_decom(netinfo_ptr, host_node_ptr);
            if (rc != 0) {
                logmsg(LOGMSG_ERROR, "reader thread: decom error from host %s\n",
                        host_node_ptr->host);
                goto done;
            }
            break;

        case WIRE_HEADER_DECOM_NAME:
            rc = process_decom_name(netinfo_ptr, host_node_ptr);
            if (rc != 0) {
                logmsg(LOGMSG_ERROR, "reader thread: decom name error from host %s\n",
                        host_node_ptr->host);
                goto done;
            }
            break;

        case WIRE_HEADER_USER_MSG:
            if (netinfo_ptr->trace && debug_switch_net_verbose())
                logmsg(LOGMSG_DEBUG, "Here %llu\n", gettmms());
            rc = process_user_message(netinfo_ptr, host_node_ptr);
            if (rc != 0) {
                logmsg(LOGMSG_ERROR, 
                        "reader thread: process_user_message error from host %s\n",
                    host_node_ptr->host);
                goto done;
            }
            break;

        case WIRE_HEADER_ACK_PAYLOAD:
            rc = process_payload_ack(netinfo_ptr, host_node_ptr);
            if (rc != 0) {
                logmsg(LOGMSG_ERROR, "reader thread: payload ack error from host %s\n",
                        host_node_ptr->host);
                goto done;
            }
            break;

        case WIRE_HEADER_ACK:
            rc = process_ack(netinfo_ptr, host_node_ptr);
            if (rc != 0) {
                logmsg(LOGMSG_ERROR, "reader thread: ack error from host %s\n",
                        host_node_ptr->host);
                goto done;
            }
            break;

        default:
            logmsg(LOGMSG_ERROR, 
                   "reader thread: unknown wire_header.type: %d from host %s\n",
                   wire_header.type, host_node_ptr->host);
            break;
        }

        if (netinfo_ptr->trace && debug_switch_net_verbose())
           logmsg(LOGMSG_USER, "RT: done processing %d %llu\n", wire_header.type,
                   gettmms());
    }

done:

    Pthread_mutex_lock(&(host_node_ptr->lock));
    host_node_ptr->have_reader_thread = 0;
    if (gbl_verbose_net)
        host_node_printf(LOGMSG_INFO, host_node_ptr, "%s exiting\n", __func__);
    close_hostnode_ll(host_node_ptr);
    Pthread_mutex_unlock(&(host_node_ptr->lock));

    if (netinfo_ptr->stop_thread_callback)
        netinfo_ptr->stop_thread_callback(netinfo_ptr->callback_data);

    return NULL;
}


#define MAXSUBNETS 15
// MAXSUBNETS + Slot for the Non-dedicated net
static char *subnet_suffices[MAXSUBNETS + 1] = {0};
static uint8_t num_dedicated_subnets = 0;
static time_t subnet_disabled[MAXSUBNETS + 1] = {0};
static int last_bad_subnet_idx = -1;
static time_t last_bad_subnet_time = 0;
uint8_t _non_dedicated_subnet = 0;
pthread_mutex_t subnet_mtx = PTHREAD_MUTEX_INITIALIZER;

int net_check_bad_subnet_lk(int ii)
{
    int rc = 0;

    if (subnet_disabled[ii])
        return 1;

    if (!last_bad_subnet_time) {
        if (gbl_verbose_net)
            logmsg(LOGMSG_USER, "%" PRIu64 " %s Not set %d %s\n",
                   pthread_self(), __func__, ii, subnet_suffices[ii]);
        goto out;
    }

    if (last_bad_subnet_time + subnet_blackout_timems < comdb2_time_epochms()) {
        if (gbl_verbose_net)
            logmsg(LOGMSG_USER, "%" PRIu64 " %s Clearing out net %d %s\n",
                   pthread_self(), __func__, ii, subnet_suffices[ii]);
        last_bad_subnet_time = 0;
        goto out;
    }

    if (ii == last_bad_subnet_idx) {
        if (gbl_verbose_net)
            logmsg(LOGMSG_USER, "%" PRIu64 " %s Bad net %d %s\n",
                   pthread_self(), __func__, ii, subnet_suffices[ii]);
        rc = 1;
    }
out:
    return rc;
}

void net_subnet_status()
{
    int i = 0;
    Pthread_mutex_lock(&subnet_mtx);
    char my_buf[30];
    for (i = 0; i < num_dedicated_subnets; i++) {
        logmsg(LOGMSG_USER, "Subnet %s %s%s%s", subnet_suffices[i],
               subnet_disabled[i] ? "disabled" : "enabled\n",
               subnet_disabled[i] ? " at " : "",
               subnet_disabled[i] ? ctime_r(&subnet_disabled[i], my_buf) : "");
    }
    Pthread_mutex_unlock(&subnet_mtx);
}

void net_set_bad_subnet(const char *subnet)
{
    int i = 0;
    Pthread_mutex_lock(&subnet_mtx);
    for (i = 0; i < num_dedicated_subnets; i++) {
        if (subnet_suffices[i][0] &&
            strncmp(subnet, subnet_suffices[i], strlen(subnet) + 1) == 0) {
            last_bad_subnet_time = comdb2_time_epochms();
            last_bad_subnet_idx = i;
            if (gbl_verbose_net)
                logmsg(LOGMSG_USER,
                       "%" PRIu64 " %s Marking %s bad, idx %d time %" PRId64
                       "\n",
                       pthread_self(), __func__, subnet_suffices[i],
                       last_bad_subnet_idx, last_bad_subnet_time);
        }
    }
    Pthread_mutex_unlock(&subnet_mtx);
}

void net_clipper(const char *subnet, int is_disable)
{
    int i = 0;
    time_t now;
    Pthread_mutex_lock(&subnet_mtx);
    for (i = 0; i < num_dedicated_subnets; i++) {
        if (subnet_suffices[i][0] &&
            strncmp(subnet, subnet_suffices[i], strlen(subnet) + 1) == 0) {
            extern int gbl_ready;
            if (gbl_ready)
                now = comdb2_time_epoch();
            else
                time(&now);
            if (gbl_verbose_net)
                logmsg(LOGMSG_USER, "0x%lx %s subnet %s time %ld\n",
                       pthread_self(), (is_disable) ? "Disabling" : "Enabling",
                       subnet_suffices[i], now);

            if (is_disable == 0) {
                subnet_disabled[i] = 0;
            } else {
                subnet_disabled[i] = now;
                kill_subnet(subnet);
            }
        }
    }
    Pthread_mutex_unlock(&subnet_mtx);
}

int net_subnet_disabled(const char *subnet)
{
    int i = 0;
    int rc = 0;
    Pthread_mutex_lock(&subnet_mtx);
    for (i = 0; i < num_dedicated_subnets; i++) {
        if (subnet_suffices[i][0] &&
            strncmp(subnet, subnet_suffices[i], strlen(subnet) + 1) == 0) {
            rc = (subnet_disabled[i] != 0);
            break;
        }
    }
    Pthread_mutex_unlock(&subnet_mtx);
    return rc;
}

int net_add_nondedicated_subnet(void *context, void *value)
{
    // increment num_dedicated_subnets only once for non dedicated subnet
    if (0 == _non_dedicated_subnet) {
        _non_dedicated_subnet = 1;
        Pthread_mutex_lock(&subnet_mtx);
        subnet_suffices[num_dedicated_subnets] = strdup("");
        num_dedicated_subnets++;
        Pthread_mutex_unlock(&subnet_mtx);
    }
    return 0;
}

int net_add_to_subnets(const char *suffix, const char *lrlname)
{
#ifdef DEBUG
    printf("net_add_to_subnets subnet '%s'\n", suffix);
#endif

    Pthread_mutex_lock(&subnet_mtx);
    if (num_dedicated_subnets >= MAXSUBNETS) {
        logmsg(LOGMSG_ERROR, "too many subnet suffices (max=%d) in lrl %s\n",
               MAXSUBNETS, lrlname);
        Pthread_mutex_unlock(&subnet_mtx);
        return -1;
    }
    subnet_suffices[num_dedicated_subnets] = strdup(suffix);
    num_dedicated_subnets++;
    Pthread_mutex_unlock(&subnet_mtx);
    return 0;
}


void net_cleanup_subnets()
{
    Pthread_mutex_lock(&subnet_mtx);
    for (uint8_t i = 0; i < num_dedicated_subnets; i++) {
        if (subnet_suffices[i]) {
            free(subnet_suffices[i]);
            subnet_suffices[i] = NULL;
        }
    }
    Pthread_mutex_unlock(&subnet_mtx);
}

/* Dedicated subnets are specified in the lrl file:
 * If option is left out, we use the normal subnet.
 * If more than one is specified, we use a counter to rotate
 * between the available dedicated subnets
 * When trying to connect, if the subnet is down
 * we will try to connect to the next one until we succeed.
 */
static int get_dedicated_conhost(host_node_type *host_node_ptr, struct in_addr *addr)
{
    static unsigned int counter = 0xffff;
    uint8_t ii = 0; // do the loop no more that max subnets

    Pthread_mutex_lock(&subnet_mtx);
    if (num_dedicated_subnets == 0) {
#ifdef DEBUG
        host_node_printf(LOGMSG_USER, host_node_ptr,
                         "Connecting to default hostname/subnet '%s'\n",
                         host_node_ptr->host);
#endif
        Pthread_mutex_unlock(&subnet_mtx);
        return comdb2_gethostbyname(&host_node_ptr->host, addr);
    }

    if (counter == 0xffff) // start with a random subnet
        counter = rand() % num_dedicated_subnets;

    int rc = 0;
    while (ii < num_dedicated_subnets) {
        counter++;
        ii++;

        const char *subnet = subnet_suffices[counter % num_dedicated_subnets];

        /* skip last bad network, if we have a choice */
        if (num_dedicated_subnets > 1) {
            if (net_check_bad_subnet_lk(counter % num_dedicated_subnets))
                continue;
        }

        char *rephostname =
            alloca(strlen(host_node_ptr->host) + strlen(subnet) + 1);
        strcpy(rephostname, host_node_ptr->host);
        if (subnet[0]) {
            strcat(rephostname, subnet);
            strncpy0(host_node_ptr->subnet, subnet, HOSTNAME_LEN);

#ifdef DEBUG
            host_node_printf(
                LOGMSG_USER, host_node_ptr,
                "Connecting to dedicated hostname/subnet '%s' counter=%d\n",
                rephostname, counter);
#endif

        }
#ifdef DEBUG
        else
            host_node_printf(
                LOGMSG_USER, host_node_ptr,
                "Connecting to NON dedicated hostname/subnet '%s' counter=%d\n",
                rephostname, counter);
#endif
        rc = comdb2_gethostbyname(&rephostname, addr);
        if (rc) {
            logmsg(LOGMSG_ERROR, "%d) %s(): ERROR gethostbyname '%s' FAILED\n",
                    ii, __func__, rephostname);
        } else {
            if (gbl_verbose_net) {
                host_node_printf(LOGMSG_USER, host_node_ptr,
                                 "'%s': gethostbyname '%s' addr %x\n", __func__,
                                 rephostname, (unsigned)addr->s_addr);
            }
            break;
        }
    }
    Pthread_mutex_unlock(&subnet_mtx);
    return rc;
}

int net_get_port_by_service(const char *dbname)
{
    short port = 0;
    comdb2_getservbyname(dbname, "tcp", &port);
    return ntohs(port);
}

static void *connect_thread(void *arg)
{
    netinfo_type *netinfo_ptr;
    host_node_type *host_node_ptr;
    int fd;
    int rc;
    int flag = 1;
    int connport = -1;

    thread_started("connect thread");
    THREAD_TYPE(__func__);

    socklen_t len;

    int flags;
    struct pollfd pfd;
    int err;

    host_node_ptr = arg;
    netinfo_ptr = host_node_ptr->netinfo_ptr;

    host_node_ptr->connect_thread_arch_tid = getarchtid();
    if (gbl_verbose_net)
        host_node_printf(LOGMSG_USER, host_node_ptr, "%s: starting tid=%d, hostname='%s'\n",
                         __func__, host_node_ptr->connect_thread_arch_tid,
                         host_node_ptr->host);

    if (netinfo_ptr->start_thread_callback)
        netinfo_ptr->start_thread_callback(netinfo_ptr->callback_data);

    struct {
        arch_tid tid;
        struct timeval t;
        pid_t pid;
    } seed_data = {0};
    seed_data.tid = getarchtid();
    seed_data.pid = getpid();

    int check = 1;
    Pthread_mutex_lock(&(host_node_ptr->lock));
    while (!host_node_ptr->decom_flag && !netinfo_ptr->exiting) {
        if (host_node_ptr->fd != -1) { /* already have connection */
            check = 1;
            goto again;
        } else if (check) {
            gettimeofday(&seed_data.t, NULL);
            uint32_t seed = crc32c((uint8_t *)&seed_data, sizeof(seed_data));
            int r = rand_r(&seed) % 5000;
            Pthread_mutex_unlock(&(host_node_ptr->lock));
            poll(NULL, 0, r);
            check = 0;
            Pthread_mutex_lock(&(host_node_ptr->lock));
            continue;
        }

        if (!host_node_ptr->really_closed) {
            goto again;
        }

        if (get_dedicated_conhost(host_node_ptr, &host_node_ptr->addr) != 0) {
            if (gbl_verbose_net)
                host_node_printf(LOGMSG_USER, host_node_ptr,
                                 "%s: couldnt connect to dedicated host\n",
                                 __func__);
            goto again;
        }

        /* *always* check portmux before connecting.  The
         * correct port may have changed since last time. */
        if (!host_node_ptr->port) {
            if (netinfo_ptr->ischild) {
                if (netinfo_ptr->use_getservbyname)
                    connport =
                        net_get_port_by_service(netinfo_ptr->parent->instance);
                if (connport <= 0)
                    connport = portmux_geti(host_node_ptr->addr,
                                            netinfo_ptr->parent->app,
                                            netinfo_ptr->parent->service,
                                            netinfo_ptr->parent->instance);
                if (connport <= 0)
                    connport = portmux_geti(
                        host_node_ptr->addr, netinfo_ptr->app,
                        netinfo_ptr->service, netinfo_ptr->instance);
            } else
                connport =
                    portmux_geti(host_node_ptr->addr, netinfo_ptr->app,
                                 netinfo_ptr->service, netinfo_ptr->instance);
        } else {
            connport = host_node_ptr->port;
        }

        if (connport <= 0) {
            host_node_printf(LOGMSG_ERROR, host_node_ptr, "portmux_geti returned port = %d\n",
                             connport);
            goto again;
        }

        struct sockaddr_in sin = {0};
        sin.sin_addr = host_node_ptr->addr;
        sin.sin_family = AF_INET;
        sin.sin_port = htons(connport);

        if (netinfo_ptr->exiting) {
            break;
        }

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            goto again;
        }

        if (gbl_verbose_net)
            host_node_printf(LOGMSG_USER, host_node_ptr, "%s: connecting on ip=%s port=%d\n",
                             __func__, inet_ntoa(sin.sin_addr),
                             ntohs(sin.sin_port));

        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            logmsgperror("tcplib:lclconn:fcntl:F_GETFL");
            exit(1);
        }
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            logmsgperror("tcplib:lclconn:fcntl:F_SETFL");
            exit(1);
        }

#ifdef NODELAY
        flag = 1;
        rc = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                        sizeof(int));
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "%s: couldnt turn off nagel on new fd %d: %d %s\n",
                    __func__, fd, errno, strerror(errno));
            exit(1);
        }
#endif
        flag = 1;
        rc = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&flag,
                        sizeof(int));
        if (rc != 0) {
            logmsg(LOGMSG_FATAL, 
                    "%s: couldnt turn on keep alive on new fd %d: %d %s\n",
                    __func__, fd, errno, strerror(errno));
            exit(1);
        }

        rc = connect(fd, (struct sockaddr *)&sin, sizeof(sin));
        if (rc == -1 && errno == EINPROGRESS) {
            /*wait for connect event */
            pfd.fd = fd;
            pfd.events = POLLOUT;

            /*fprintf(stderr, "sleeping for 100ms\n");*/

            rc = poll(&pfd, 1, 100);

            if (rc == 0) {
                /*timeout*/
                host_node_printf(LOGMSG_WARN, host_node_ptr, "%s: connect timed out\n",
                                 __func__);
                close(fd);
                goto again;
            }
            if (rc != 1) {
                /*poll failed?*/
                host_node_printf(LOGMSG_ERROR, host_node_ptr,
                                 "%s: poll on connect failed %d %s\n", __func__,
                                 errno, strerror(errno));
                close(fd);
                goto again;
            }
            if ((pfd.revents & POLLOUT) == 0) {
#ifdef _LINUX_SOURCE
                /* Linux returns EPOLLHUP|EPOLLERR */
                host_node_printf(LOGMSG_WARN, host_node_ptr,
                                 "%s: poll returns events 0x%03x\n", __func__,
                                 pfd.revents);
#else
                /*wrong event*/
                host_node_printf(LOGMSG_ERROR, host_node_ptr,
                                 "%s: poll returned wrong event\n", __func__);
#endif
                close(fd);
                goto again;
            }

            if (fcntl(fd, F_SETFL, flags) < 0) {
                logmsgperror("tcplib:lclconn:fcntl2");
                exit(1);
            }
        } else if (rc == -1) {
            if (gbl_verbose_net)
                host_node_printf(LOGMSG_USER, host_node_ptr, "%s: connect error %d %s\n",
                                 __func__, errno, strerror(errno));
            close(fd);
            goto again;
        } else {
            if (gbl_verbose_net)
                host_node_printf(LOGMSG_USER, host_node_ptr, "%s: connect succeeded\n",
                                 __func__);
        }

        /* put blocking back */
        if (fcntl(fd, F_SETFL, flags) < 0) {
            logmsgperror("tcplib:lclconn:fcntl2");
            exit(1);
        }

        len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len)) {
            logmsgperror("tcplib:lclconn:getsockopt");
#ifndef _HP_SOURCE
            exit(1);
#endif
        }

        /* remember the port */
        host_node_ptr->port = connport;

        host_node_ptr->sb = sbuf2open(fd, SBUF2_NO_CLOSE_FD | SBUF2_NO_FLUSH);
        if (host_node_ptr->sb == NULL) {
            host_node_errf(LOGMSG_ERROR, host_node_ptr, "%s: sbuf2open failed\n", __func__);
            close(fd);
            goto again;
        }

        sbuf2setbufsize(host_node_ptr->sb, netinfo_ptr->bufsz);

        if (debug_switch_net_verbose()) {
            logmsg(LOGMSG_USER, "Setting wrapper\n");
            /* override sbuf2 defaults for testing */
            sbuf2setr(host_node_ptr->sb, sbuf2read_wrapper);
            sbuf2setw(host_node_ptr->sb, sbuf2write_wrapper);
        }

        /* doesn't matter - it's under lock ... */
        host_node_ptr->timestamp = time(NULL);

        Pthread_mutex_lock(&(host_node_ptr->write_lock));

        if (gbl_verbose_net)
            host_node_printf(LOGMSG_USER, host_node_ptr, "%s: write connect message\n",
                             __func__);
        rc = write_connect_message(netinfo_ptr, host_node_ptr,
                                   host_node_ptr->sb);
        if (rc != 0) {
            host_node_printf(LOGMSG_ERROR, host_node_ptr,
                             "%s: couldn't send connect message\n", __func__);
            Pthread_mutex_unlock(&(host_node_ptr->write_lock));
            close_hostnode_ll(host_node_ptr);
            goto again;
        }
        sbuf2flush(host_node_ptr->sb);

        /*
           dont set this till after we wrote the connect message -
           this prevents a race where the heartbeat thread gets the
           heartbeat out before we get the connect message out
        */
        host_node_ptr->fd = fd;
        host_node_ptr->really_closed = 0;
        host_node_ptr->closed = 0;

        /* Also call the new node routine here - it shouldn't matter which
         * node initiated the connection. */
        if (netinfo_ptr->new_node_rtn)
            netinfo_ptr->new_node_rtn(netinfo_ptr, host_node_ptr->host, host_node_ptr->port);

        /* wake writer, if exists */
        Pthread_cond_signal(&(host_node_ptr->write_wakeup));
        Pthread_mutex_unlock(&(host_node_ptr->write_lock));

        if (gbl_verbose_net)
            host_node_printf(LOGMSG_USER, 
                    host_node_ptr, "%s: connection established\n",
                             __func__);
        host_node_ptr->timestamp = time(NULL);

        rc = create_reader_writer_threads(host_node_ptr, __func__);
        if (rc != 0) {
            close_hostnode_ll(host_node_ptr);
            goto again;
        }

    again:
        if (netinfo_ptr->exiting) {
            break;
        }
        Pthread_mutex_unlock(&(host_node_ptr->lock));
        sleep(1);
        Pthread_mutex_lock(&(host_node_ptr->lock));
    }
    host_node_ptr->have_connect_thread = 0;

    if (host_node_ptr->decom_flag)
        logmsg(LOGMSG_INFO, "connect_thread: host_node_ptr->decom_flag set for host %s\n",
                host_node_ptr->host);
    else
        host_node_printf(LOGMSG_INFO, host_node_ptr, "connect_thread: netinfo->exiting\n");

    /* close the file-descriptor, wait for reader / writer threads
       to exit, free host_node_ptr, then exit */
    close_hostnode_ll(host_node_ptr);
    Pthread_mutex_unlock(&(host_node_ptr->lock));

    while (!netinfo_ptr->exiting) {
        int ref;
        Pthread_mutex_lock(&(host_node_ptr->lock));
        ref = host_node_ptr->have_reader_thread +
              host_node_ptr->have_writer_thread;
        Pthread_mutex_unlock(&(host_node_ptr->lock));

        Pthread_mutex_lock(&(host_node_ptr->throttle_lock));
        ref += host_node_ptr->throttle_waiters;
        if (host_node_ptr->throttle_waiters > 0)
            Pthread_cond_broadcast(&(host_node_ptr->throttle_wakeup));
        Pthread_mutex_unlock(&(host_node_ptr->throttle_lock));

        if (ref == 0)
            break;

        Pthread_cond_signal(&(host_node_ptr->write_wakeup));
        poll(NULL, 0, 1000);
    }

    if (!netinfo_ptr->exiting) {
        /* lock, unlink, free, damn it */
        rem_from_netinfo(netinfo_ptr, host_node_ptr);
    }

    if (netinfo_ptr->stop_thread_callback)
        netinfo_ptr->stop_thread_callback(netinfo_ptr->callback_data);

    return NULL;
}


/*  changed to support a 'sponsor-host' trace: if we're starting a connect-
    thread because of machine we learned about from a recent 'hello' message,
    make that information explicit here */
static int connect_to_host(netinfo_type *netinfo_ptr,
                           host_node_type *host_node_ptr,
                           host_node_type *sponsor_host)
{
    if (host_node_ptr->host == netinfo_ptr->myhostname)
        return 1;

    Pthread_mutex_lock(&(host_node_ptr->lock));
    if (host_node_ptr->have_connect_thread == 0) {
        if (gbl_verbose_net) {
            if (sponsor_host) {
                host_node_printf(
                    LOGMSG_DEBUG,
                    sponsor_host,
                    "%s: creating (sponsored) connect_thread for %s:%d\n",
                    __func__, host_node_ptr->host, host_node_ptr->port);
            } else {
                logmsg(LOGMSG_INFO, "%s: creating connect_thread for node %s:%d\n",
                        __func__, host_node_ptr->host, host_node_ptr->port);
            }
        }

        int rc = pthread_create(&(host_node_ptr->connect_thread_id),
                                &(netinfo_ptr->pthread_attr_detach),
                                connect_thread, host_node_ptr);
        if (rc != 0) {
            if (sponsor_host) {
                host_node_errf(LOGMSG_ERROR, sponsor_host,
                               "%s: couldnt create connect thd, errno=%d %s\n",
                               __func__, errno, strerror(errno));
            } else {
                logmsg(LOGMSG_ERROR, "%s: couldnt create connect thd, errno=%d %s\n",
                        __func__, errno, strerror(errno));
            }
            Pthread_mutex_unlock(&(host_node_ptr->lock));
            return -1;
        }
        host_node_ptr->have_connect_thread = 1;
    }
    Pthread_mutex_unlock(&(host_node_ptr->lock));
    return 0;
}

static int get_subnet_incomming_syn(host_node_type *host_node_ptr)
{
    struct sockaddr_in lcl_addr_inet;
    socklen_t lcl_len = sizeof(lcl_addr_inet);
    if (getsockname(host_node_ptr->fd, (struct sockaddr *)&lcl_addr_inet,
                    &lcl_len)) {
        logmsg(LOGMSG_ERROR, "Failed to getsockname() for fd=%d\n",
               host_node_ptr->fd);
        return 0;
    }

    char host[NI_MAXHOST], service[NI_MAXSERV];
    /* get our host name for local _into_ address of connection */
    int s = getnameinfo((struct sockaddr *)&lcl_addr_inet, lcl_len, host,
                        NI_MAXHOST, service, NI_MAXSERV, 0);

    if (s != 0) {
        logmsg(LOGMSG_WARN, "Incoming connection into unknown (%s:%u): %s\n",
               inet_ntoa(lcl_addr_inet.sin_addr),
               (unsigned)ntohs(lcl_addr_inet.sin_port), gai_strerror(s));
        return 0;
    }

    /* extract the suffix of subnet ex. '_n3' in name node1_n3 */
    int myh_len = strlen(host_node_ptr->netinfo_ptr->myhostname);
    if (strncmp(host_node_ptr->netinfo_ptr->myhostname, host, myh_len) == 0) {
        assert(myh_len <= sizeof(host));
        char *subnet = &host[myh_len];
        if (subnet[0])
            strncpy0(host_node_ptr->subnet, subnet, HOSTNAME_LEN);
    }

    /* check if the net is disabled */
    if (net_subnet_disabled(host_node_ptr->subnet))
        return 1;

    return 0;
}

static void accept_handle_new_host(netinfo_type *netinfo_ptr,
                                   const char *hostname, int portnum,
                                   int new_fd, SBUF2 *sb, struct in_addr addr)
{
    host_node_type *host_node_ptr = NULL;

#ifdef DEBUG
    char ip[16];
    fprintf(stderr, "%s: hostname %s (%s:%d)\n", __func__, hostname,
            inet_ntoa_r(addr.s_addr, ip), portnum);
#endif

    /* dont accept when we are exiting (how about decom?) */
    if (netinfo_ptr->exiting) {
        logmsg(LOGMSG_INFO, "%s:we are exiting so not continuing conn to %s\n",
                __func__, hostname);
        return;
    }

    /* see if we already have an entry.  if we do, CLOSE the socket.
       if we dont, create a reader_thread */
    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    host_node_ptr = get_host_node_by_name_ll(netinfo_ptr, hostname);
    if (!host_node_ptr) {
        /* unlock */
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));

        /* add to netinfo */
        host_node_ptr = add_to_netinfo(netinfo_ptr, hostname, portnum);

        /* failed to add .. sbuf has NO_CLOSE_FD set */
        if (host_node_ptr == NULL) {
            sbuf2close(sb);
            close(new_fd);
            return;
        }

        /* relock */
        Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

        /* find */
        host_node_ptr = get_host_node_by_name_ll(netinfo_ptr, hostname);

        /* removed from under us .. */
        if (host_node_ptr == NULL) {
            sbuf2close(sb);
            close(new_fd);
            Pthread_rwlock_unlock(&(netinfo_ptr->lock));
            return;
        }

        /* success! */
        if (gbl_verbose_net)
            host_node_printf(
                LOGMSG_USER,
                host_node_ptr,
                "%s: got initial connection on fd %d - new host_node_ptr\n",
                __func__, new_fd);
    }

    if (gbl_verbose_net)
        host_node_printf(LOGMSG_USER, host_node_ptr, "%s: got initial connection on fd %d\n",
                         __func__, new_fd);

    int cnt = 0;
    Pthread_mutex_lock(&(host_node_ptr->lock));
    while (1) {
        if (netinfo_ptr->exiting || host_node_ptr->decom_flag) {
            sbuf2close(sb);
            close(new_fd);
            Pthread_mutex_unlock(&(host_node_ptr->lock));
            Pthread_rwlock_unlock(&(netinfo_ptr->lock));
            return;
        }
        if (!host_node_ptr->have_reader_thread &&
            !host_node_ptr->have_writer_thread) {
            break;
        }
        /* shutdown the fd. rd/wr thds (if any) will stop immediately */
        close_hostnode_ll(host_node_ptr);
        Pthread_mutex_unlock(&(host_node_ptr->lock));
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        poll(NULL, 0, 100);
        if (++cnt % 10 == 0) {
            host_node_printf(LOGMSG_USER, host_node_ptr,
                             "%s waiting for r/w thds to exit\n", __func__);
        }
        Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
        Pthread_mutex_lock(&(host_node_ptr->lock));
    }

    /* Set the port so that we can send out messages that the other end
     * will understand (i.e. no packet not intended for us silliness) */
    if (host_node_ptr->port != portnum) {
        host_node_printf(LOGMSG_DEBUG, host_node_ptr, "%s: changing port from %d to %d\n",
                         __func__, host_node_ptr->port, portnum);
        host_node_ptr->port = portnum;
    }

    /* assigning addr here is needed to do udp_send()
     * if connect_thread body gets executed (when there is no connection yet)
     * it will refresh the addr (and that's ok).
     */
    host_node_ptr->addr = addr;
    memset(host_node_ptr->subnet, 0, HOSTNAME_LEN);

    host_node_ptr->timestamp = time(NULL);
    Pthread_mutex_lock(&(host_node_ptr->write_lock));
    empty_write_list(host_node_ptr);
    host_node_ptr->fd = new_fd;
    host_node_ptr->sb = sb;
    Pthread_mutex_unlock(&(host_node_ptr->write_lock));

    int rc = get_subnet_incomming_syn(host_node_ptr);
    if (rc) {
        host_node_printf(LOGMSG_INFO, host_node_ptr,
                         "%s: Clipping connect from %s on disabled subnet %s\n",
                         __func__, host_node_ptr->host,
                         (host_node_ptr->subnet[0] != '\0')
                             ? host_node_ptr->subnet
                             : "UNKNOWN");
        Pthread_mutex_unlock(&(host_node_ptr->lock));
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        return;
    }

    if (gbl_verbose_net)
        host_node_errf(LOGMSG_USER, host_node_ptr, "%s: accepting connection on new_fd %d\n",
                       __func__, new_fd);

    host_node_ptr->really_closed = 0;
    host_node_ptr->closed = 0;
    host_node_ptr->rej_up_cnt = 0;

    /* create reader & writer threads */
    rc = create_reader_writer_threads(host_node_ptr, __func__);
    if (rc != 0) {
        close_hostnode_ll(host_node_ptr);
        Pthread_mutex_unlock(&(host_node_ptr->lock));
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        return;
    }

    int become_connect_thread = 0;

    /* become the connect thread if we don't have one */
    if (!(host_node_ptr->have_connect_thread)) {
        /* set a flag */
        become_connect_thread = 1;

        /* prevent this from being created elsewhere */
        host_node_ptr->have_connect_thread = 1;

        /* set my id */
        host_node_ptr->connect_thread_id = pthread_self();
    }

    if (host_node_ptr->distress) {
        unsigned cycles = host_node_ptr->distress;
        host_node_ptr->distress = 0;
        host_node_printf(LOGMSG_INFO, 
                         host_node_ptr,
                         "%s: leaving distress mode after %u cycles\n",
                         __func__, cycles);
    }

    Pthread_mutex_unlock(&(host_node_ptr->lock));

    // Why write hello here? writer thread above will say hello, no?
    /* write_hello(netinfo_ptr, host_node_ptr); */

    if (gbl_verbose_net)
        host_node_printf(LOGMSG_USER, host_node_ptr, "%s: wrote hello\n", __func__);

    /* call the newhost routine if provided */
    if (host_node_ptr->netinfo_ptr->new_node_rtn) {
        (host_node_ptr->netinfo_ptr->new_node_rtn)(host_node_ptr->netinfo_ptr,
                                                   host_node_ptr->host,
                                                   host_node_ptr->port);
        host_node_printf(LOGMSG_DEBUG, host_node_ptr, "back from newnode_rtn\n");
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    /* become the connect thread */
    if (become_connect_thread) {
        if (gbl_verbose_net)
            host_node_printf(LOGMSG_USER, host_node_ptr, "becomming connect thread\n");
        connect_thread(host_node_ptr);
    }

    ++netinfo_ptr->num_current_non_appsock_accepts;
}


/* find the remote peer.  code stolen from sqlinterfaces.c */
static inline int findpeer(int fd, char *addr, int len)
{
    int rc;
    struct sockaddr_in peeraddr;
    socklen_t pl = sizeof(struct sockaddr_in);

    /* find peer ip */
    rc = getpeername(fd, (struct sockaddr *)&peeraddr, &pl);
    if (rc) {
        snprintf(addr, len, "<unknown>");
        return -1;
    }

    /* find hostname */
    if (NULL == inet_ntop(peeraddr.sin_family, &peeraddr.sin_addr, addr, len)) {
        snprintf(addr, len, "<unknown>");
        return -1;
    }

    return 0;
}


/* reads the connect message & creates threads to monitor connection state */
static void *connect_and_accept(void *arg)
{
    connect_and_accept_t *ca;
    netinfo_type *netinfo_ptr;
    SBUF2 *sb;
    char hostname[256], addr[64];
    int portnum = 0, fd, rc;
    char *host;
    int netnum;

    /* retrieve arguments */
    ca = (connect_and_accept_t *)arg;
    netinfo_ptr = ca->netinfo_ptr;
    sb = ca->sb;
    fd = sbuf2fileno(sb);
    struct in_addr inaddr = ca->addr;

    /* free */
    Pthread_mutex_lock(&(netinfo_ptr->connlk));
    pool_relablk(netinfo_ptr->connpool, ca);
    Pthread_mutex_unlock(&(netinfo_ptr->connlk));

    /* read connect message */
    rc = read_connect_message(sb, hostname, sizeof(hostname), &portnum,
                              netinfo_ptr);
    host = intern(hostname);

    /* print the origin of malformed messages */
    if (rc < 0) {
        findpeer(fd, addr, sizeof(addr));
        logmsg(LOGMSG_ERROR, "%s:malformed connect message from %s "
                        "closing connection\n",
                __func__, addr);
        sbuf2close(sb);
        pthread_exit(NULL);
    }

    // This is our socket, not an appsock. Set the flags we want.
    sbuf2setflags(sb, SBUF2_NO_CLOSE_FD | SBUF2_NO_FLUSH);
    netnum = (portnum & 0x000f0000) >> 16;
    /* Special port number to indicate we're meant for a different net. */
    portnum &= 0xffff;
    /* if connect message specifies a child net, use it */
    if (netnum && netnum != netinfo_ptr->netnum) {
        netinfo_type *net;
        Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
        if (netnum < 0 || netnum >= netinfo_ptr->num_child_nets ||
            netinfo_ptr->child_nets[netnum] == NULL) {
            logmsg(LOGMSG_ERROR,
                   "connect message for netnum %d, num_child_nets %d, net not "
                   "registered\n",
                   netnum, netinfo_ptr->num_child_nets);
            Pthread_rwlock_unlock(&(netinfo_ptr->lock));
            return NULL;
        }
        net = netinfo_ptr->child_nets[netnum];
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));
        accept_handle_new_host(net, host, portnum, fd, sb, inaddr);
    } else {
        /* i become the connect thread if there isn't one already */
        accept_handle_new_host(netinfo_ptr, host, portnum, fd, sb, inaddr);
    }

    return NULL;
}


void net_register_child_net(netinfo_type *netinfo_ptr,
                            netinfo_type *netinfo_child, int netnum, int accept)
{
    netinfo_type **t;
    Pthread_rwlock_wrlock(&(netinfo_ptr->lock));
    if (netnum >= netinfo_ptr->num_child_nets) {
        t = calloc(netnum + 1, sizeof(netinfo_type *));
        if (t == NULL) {
            logmsg(LOGMSG_FATAL, "Can't allocate memory for child net\n");
            abort();
        }
        for (int i = 0; i < netinfo_ptr->num_child_nets; i++)
            t[i] = netinfo_ptr->child_nets[i];
        netinfo_ptr->child_nets = t;
    }
    netinfo_child->parent = netinfo_ptr;
    netinfo_child->netnum = netnum;
    netinfo_child->accept_on_child = accept;
    netinfo_ptr->child_nets[netinfo_child->netnum] = netinfo_child;
    if (netnum > netinfo_ptr->num_child_nets)
        netinfo_ptr->num_child_nets = netnum + 1;
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

int gbl_forbid_remote_admin = 1;

static void *accept_thread(void *arg)
{
    netinfo_type *netinfo_ptr;
    struct pollfd pol;
    int rc;
    int listenfd = 0;
    int polltm;
    int tcpbfsz;
    struct linger linger_data;
    struct sockaddr_in cliaddr;
    connect_and_accept_t *ca;
    pthread_t tid;
    char paddr[64];
    socklen_t clilen;
    int new_fd;
    int flag = 1;
    SBUF2 *sb;
    portmux_fd_t *portmux_fds = NULL;
    watchlist_node_type *watchlist_node;
    unsigned int last_stat_dump_time = comdb2_time_epochms();

    thread_started("net accept");
    THREAD_TYPE(__func__);

    netinfo_ptr = (netinfo_type *)arg;

    netinfo_ptr->accept_thread_arch_tid = getarchtid();

    logmsg(LOGMSG_INFO, "%s: starting, tid=%d.\n", __func__,
            netinfo_ptr->accept_thread_arch_tid);

    if (netinfo_ptr->start_thread_callback)
        netinfo_ptr->start_thread_callback(netinfo_ptr->callback_data);

    logmsg(LOGMSG_INFO, "net %s my port is %d fd is %d\n", netinfo_ptr->service,
            netinfo_ptr->myport, netinfo_ptr->myfd);

    if (gbl_pmux_route_enabled) {
        portmux_fds =
            portmux_listen_setup(netinfo_ptr->app, netinfo_ptr->service,
                                 netinfo_ptr->instance, netinfo_ptr->myfd);
        if (!portmux_fds) {
            logmsg(LOGMSG_FATAL, "Could not get portmux_fds\n");
            exit(1);
        }
    } else {
        /* We used to listen here. We now listen way earlier and get a file
           descriptor passed in.
           This is to prevent 2 instances from coming up against the same data.
           */
        if (netinfo_ptr->myfd != -1)
            listenfd = netinfo_ptr->myfd;
        else
            listenfd = netinfo_ptr->myfd = net_listen(netinfo_ptr->myport);
    }

    netinfo_ptr->accept_thread_created = 1;
    /*fprintf(stderr, "setting netinfo_ptr->accept_thread_created\n");*/

    while (!netinfo_ptr->exiting) {

        clilen = sizeof(cliaddr);

        if (portmux_fds) {
            new_fd = portmux_accept(portmux_fds, -1);
        } else {
            new_fd = accept(listenfd, (struct sockaddr *)&cliaddr, &clilen);
        }
        if (new_fd == 0 || new_fd == 1 || new_fd == 2) {
            logmsg(LOGMSG_ERROR, "Weird new_fd:%d\n", new_fd);
        }

        if (new_fd == -1) {
            logmsg(LOGMSG_ERROR, "accept fd %d rc %d %s", listenfd, errno,
                    strerror(errno));
            continue;
        }

        if(portmux_fds) {
            rc = getpeername(new_fd, (struct sockaddr *)&cliaddr, &clilen);
            if (rc) {
                logmsg(LOGMSG_ERROR,
                       "Failed to get peer address, error: %d %s\n", errno,
                       strerror(errno));
                close(new_fd);
                continue;
            }
        }

        if (netinfo_ptr->exiting) {
            close(new_fd);
            break;
        }

#ifdef NODELAY
        /* We've seen unexplained EINVAL errors here.  Be extremely defensive
         * and always reset flag to 1 before calling this function. */
        flag = 1;
        rc = setsockopt(new_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                        sizeof(int));
        /* Note: don't complain on EINVAL.  There's a legitimate condition where
           the requester drops the socket according to manpages. */
        if (rc != 0 && errno != EINVAL) {
            logmsg(LOGMSG_ERROR, 
                    "%s: couldnt turn off nagel on new_fd %d, flag=%d: %d "
                    "%s\n",
                    __func__, new_fd, flag, errno, strerror(errno));
            close(new_fd);
            continue;
        }
#endif

        flag = 1;
        rc = setsockopt(new_fd, SOL_SOCKET, SO_KEEPALIVE, (char *)&flag,
                        sizeof(int));
        if (rc != 0) {
            logmsg(LOGMSG_FATAL, "%s: couldnt turn on keep alive on new fd %d: %d %s\n",
                    __func__, new_fd, errno, strerror(errno));
            exit(1);
        }

#ifdef TCPBUFSZ
        tcpbfsz = (8 * 1024 * 1024);
        rc = setsockopt(new_fd, SOL_SOCKET, SO_SNDBUF, &tcpbfsz,
                        sizeof(tcpbfsz));
        if (rc < 0) {
            logmsg(LOGMSG_FATAL, "%s: couldnt set tcp sndbuf size on listenfd %d: %d %s\n",
                    __func__, new_fd, errno, strerror(errno));
            exit(1);
        }

        tcpbfsz = (8 * 1024 * 1024);
        rc = setsockopt(new_fd, SOL_SOCKET, SO_RCVBUF, &tcpbfsz,
                        sizeof(tcpbfsz));
        if (rc < 0) {
            logmsg(LOGMSG_FATAL, 
                    "%s: couldnt set tcp rcvbuf size on listenfd %d: %d %s\n",
                    __func__, new_fd, errno, strerror(errno));
            exit(1);
        }
#endif

#ifdef NOLINGER
        linger_data.l_onoff = 0;
        linger_data.l_linger = 1;
        if (setsockopt(new_fd, SOL_SOCKET, SO_LINGER, (char *)&linger_data,
                       sizeof(linger_data)) != 0) {
            logmsg(LOGMSG_ERROR, "%s: couldnt turn off linger on new_fd %d: %d %s\n",
                    __func__, new_fd, errno, strerror(errno));
            close(new_fd);
            continue;
        }
#endif

        /* get a buffered pointer to the socket */
        sb = sbuf2open(new_fd, 0); /* no flags yet... */
        if (sb == NULL) {
            logmsg(LOGMSG_ERROR, "sbuf2open failed\n");
            continue;
        }

        if (debug_switch_net_verbose()) {
            logmsg(LOGMSG_DEBUG, "Setting wrapper\n");
            sbuf2setr(sb, sbuf2read_wrapper);
            sbuf2setw(sb, sbuf2write_wrapper);
        }

        sbuf2setbufsize(sb, netinfo_ptr->bufsz);

        /* reasonable default for poll */
        polltm = 100;

        /* use tuned value if set */
        if (netinfo_ptr->netpoll > 0) {
            polltm = netinfo_ptr->netpoll;
        }

        /* setup poll */
        pol.fd = new_fd;
        pol.events = POLLIN;

        /* poll */
        unsigned pollstart, pollend;
        pollstart = comdb2_time_epochms();
        rc = poll(&pol, 1, polltm);
        pollend = comdb2_time_epochms();

        quantize(netinfo_ptr->conntime_all, pollend - pollstart);
        quantize(netinfo_ptr->conntime_periodic, pollend - pollstart);
        netinfo_ptr->num_accepts++;

        if (netinfo_ptr->conntime_dump_period && ((pollend - last_stat_dump_time) / 1000) > netinfo_ptr->conntime_dump_period ) {
            quantize_ctrace(netinfo_ptr->conntime_all, "Accept poll times, overall:");
            quantize_ctrace(netinfo_ptr->conntime_periodic, "Accept poll times, last period:");
            quantize_clear(netinfo_ptr->conntime_periodic);
            last_stat_dump_time = pollend;
        }

        netinfo_ptr->num_accepts++;

        /* drop connection on poll error */
        if (rc < 0) {
            findpeer(new_fd, paddr, sizeof(paddr));
            logmsg(LOGMSG_ERROR, "%s: error from poll: %s, peeraddr=%s\n", __func__,
                    strerror(errno), paddr);
            sbuf2close(sb);
            continue;
        }

        /* drop connection on timeout */
        else if (0 == rc) {
            findpeer(new_fd, paddr, sizeof(paddr));
            logmsg(LOGMSG_ERROR, "%s: timeout reading from socket, peeraddr=%s\n",
                    __func__, paddr);
            sbuf2close(sb);
            netinfo_ptr->num_accept_timeouts++;
            continue;
        }

        /* drop connection if i would block in read */
        if ((pol.revents & POLLIN) == 0) {
            findpeer(new_fd, paddr, sizeof(paddr));
            logmsg(LOGMSG_ERROR, "%s: cannot read without blocking, peeraddr=%s\n",
                    __func__, paddr);
            sbuf2close(sb);
            continue;
        }

        /* the above poll ensures that this will not block */

        uint8_t firstbyte;
        rc = read_stream(netinfo_ptr, NULL, sb, &firstbyte, 1);
        if (rc != 1) {
            findpeer(new_fd, paddr, sizeof(paddr));
            logmsg(LOGMSG_ERROR, "%s: read_stream failed for = %s\n", __func__,
                    paddr);
            sbuf2close(sb);
            continue;
        }

        /* appsock reqs have a non-0 first byte */
        if (firstbyte > 0) {
            int admin = 0;
            APPSOCKFP *rtn = NULL;

            if (firstbyte == '@') {
                findpeer(new_fd, paddr, sizeof(paddr));
                if (!gbl_forbid_remote_admin ||
                    (cliaddr.sin_addr.s_addr == htonl(INADDR_LOOPBACK))) {
                    logmsg(LOGMSG_INFO, "Accepting admin user from %s\n",
                           paddr);
                    admin = 1;
                } else {
                    logmsg(LOGMSG_INFO,
                           "Rejecting non-local admin user from %s\n", paddr);
                    sbuf2close(sb);
                    continue;
                }
            } else if (firstbyte != sbuf2ungetc(firstbyte, sb)) {
                logmsg(LOGMSG_ERROR, "sbuf2ungetc failed %s:%d\n", __FILE__,
                        __LINE__);
                sbuf2close(sb);
                continue;
            }

            /* call user specified app routine */
            if (admin && netinfo_ptr->admin_appsock_rtn) {
                rtn = netinfo_ptr->admin_appsock_rtn;
            } else if (netinfo_ptr->appsock_rtn) {
                rtn = netinfo_ptr->appsock_rtn;
            }

            if (rtn) {
                /* set up the watchlist system for this node */
                watchlist_node = calloc(1, sizeof(watchlist_node_type));
                if (!watchlist_node) {
                    logmsg(LOGMSG_ERROR, "%s: malloc watchlist_node failed\n",
                            __func__);
                    sbuf2close(sb);
                    continue;
                }
                memcpy(watchlist_node->magic, "WLST", 4);
                watchlist_node->in_watchlist = 0;
                watchlist_node->netinfo_ptr = netinfo_ptr;
                watchlist_node->sb = sb;
                watchlist_node->readfn = sbuf2getr(sb);
                watchlist_node->writefn = sbuf2getw(sb);
                watchlist_node->addr = cliaddr;
                sbuf2setrw(sb, net_reads, net_writes);
                sbuf2setuserptr(sb, watchlist_node);

                /* this doesn't read- it just farms this off to a thread */
                (rtn)(netinfo_ptr, sb);
            }

            continue;
        }

        /* grab pool memory for connect_and_accept_t */
        Pthread_mutex_lock(&(netinfo_ptr->connlk));
        ca = (connect_and_accept_t *)pool_getablk(netinfo_ptr->connpool);
        Pthread_mutex_unlock(&(netinfo_ptr->connlk));

        /* setup connect_and_accept args */
        ca->netinfo_ptr = netinfo_ptr;
        ca->sb = sb;
        ca->addr = cliaddr.sin_addr;

        /* connect and accept- this might be replaced with a threadpool later */
        rc = pthread_create(&tid, &(netinfo_ptr->pthread_attr_detach),
                            connect_and_accept, ca);

        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "%s:pthread_create error: %s\n", __func__,
                    strerror(errno));
            Pthread_mutex_lock(&(netinfo_ptr->connlk));
            pool_relablk(netinfo_ptr->connpool, ca);
            Pthread_mutex_unlock(&(netinfo_ptr->connlk));
            sbuf2close(sb);
            continue;
        }
    }

    close(listenfd);

#ifdef NOTREACHED
    if (netinfo_ptr->stop_thread_callback)
        netinfo_ptr->stop_thread_callback(netinfo_ptr->callback_data);

    if (portmux_fds)
        portmux_close(portmux_fds);
#endif
    /* make the compiler shutup */
    return NULL;
}


static void *heartbeat_send_thread(void *arg)
{
    host_node_type *ptr;
    netinfo_type *netinfo_ptr;

    thread_started("net heartbeat send");

    netinfo_ptr = (netinfo_type *)arg;

    netinfo_ptr->heartbeat_send_thread_arch_tid = getarchtid();
    logmsg(LOGMSG_INFO, "heartbeat send thread starting.  time=%d.  tid=%d\n",
            netinfo_ptr->heartbeat_send_time,
            netinfo_ptr->heartbeat_send_thread_arch_tid);

    if (netinfo_ptr->start_thread_callback)
        netinfo_ptr->start_thread_callback(netinfo_ptr->callback_data);

    while (!netinfo_ptr->exiting) {
        /* netinfo lock protects the list AND the write_heartbeat call
           no need to grab rdlock */
        Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
        for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
            if (ptr->host != netinfo_ptr->myhostname) {
                write_heartbeat(netinfo_ptr, ptr);
            }
        }
        Pthread_rwlock_unlock(&(netinfo_ptr->lock));

        if (netinfo_ptr->exiting)
            break;

        sleep(netinfo_ptr->heartbeat_send_time);
    }
    if (netinfo_ptr->stop_thread_callback)
        netinfo_ptr->stop_thread_callback(netinfo_ptr->callback_data);

    return NULL;
}

static watchlist_node_type *get_watchlist_node(SBUF2 *sb, const char *funcname)
{
    watchlist_node_type *watchlist_node = sbuf2getuserptr(sb);
    if (!watchlist_node) {
        logmsg(LOGMSG_ERROR, "%s: sbuf2 %p has no user pointer\n", funcname, sb);
        return NULL;
    } else if (memcmp(watchlist_node->magic, "WLST", 4) != 0) {
        logmsg(LOGMSG_ERROR, "%s: sbuf2 %p user pointer is not a watch list node\n",
                funcname, sb);
        return NULL;
    } else {
        return watchlist_node;
    }
}

static int net_writes(SBUF2 *sb, const char *buf, int nbytes)
{
    int outrc;
    watchlist_node_type *watchlist_node = get_watchlist_node(sb, __func__);
    if (!watchlist_node)
        return -1;
    watchlist_node->write_age = comdb2_time_epoch();
    outrc = watchlist_node->writefn(sb, buf, nbytes);
    watchlist_node->write_age = 0;
    return outrc;
}

static int net_reads(SBUF2 *sb, char *buf, int nbytes)
{
    int outrc;
    watchlist_node_type *watchlist_node = get_watchlist_node(sb, __func__);
    if (!watchlist_node)
        return -1;
    watchlist_node->read_age = comdb2_time_epoch();
    outrc = watchlist_node->readfn(sb, buf, nbytes);
    watchlist_node->read_age = 0;
    return outrc;
}

void net_timeout_watchlist(netinfo_type *netinfo_ptr)
{
    watchlist_node_type *watchlist_ptr;
    SBUF2 *sb;
    int fd;

    Pthread_mutex_lock(&(netinfo_ptr->watchlk));

    LISTC_FOR_EACH(&(netinfo_ptr->watchlist), watchlist_ptr, lnk)
    {
        sb = watchlist_ptr->sb;
        fd = sbuf2fileno(sb);

        int write_age = watchlist_ptr->write_age;
        int read_age = watchlist_ptr->read_age;

        if (((watchlist_ptr->write_timeout) && (write_age) &&
             ((comdb2_time_epoch() - write_age) >
              watchlist_ptr->write_timeout)) ||

            ((watchlist_ptr->read_timeout) && (read_age) &&
             ((comdb2_time_epoch() - read_age) >
              watchlist_ptr->read_timeout))) {
            logmsg(LOGMSG_INFO, "timing out session, closing fd %d read_age %d "
                                "timeout %d write_age %d timeout %d\n",
                   fd, comdb2_time_epoch() - read_age,
                   watchlist_ptr->read_timeout, comdb2_time_epoch() - write_age,
                   watchlist_ptr->write_timeout);
            shutdown(fd, 2);

            watchlist_ptr->write_timeout = 0;
            watchlist_ptr->read_timeout = 0;
        }
        /* warning path */
        else if ((watchlist_ptr->read_warning_timeout) &&
                 (watchlist_ptr->read_warning_arg) &&
                 (watchlist_ptr->read_warning_func) && (read_age)) {
            int gap = comdb2_time_epoch() - read_age;
            if (gap > watchlist_ptr->read_warning_timeout) {
                int rc = watchlist_ptr->read_warning_func(
                    watchlist_ptr->read_warning_arg,
                    watchlist_ptr->read_warning_timeout, gap);
                if (rc < 0) {
                    logmsg(LOGMSG_INFO, "timing out session, closing fd %d\n", fd);
                    shutdown(fd, 2);

                    watchlist_ptr->write_timeout = 0;
                    watchlist_ptr->read_timeout = 0;
                    watchlist_ptr->read_warning_timeout = 0;
                } else if (rc == 1) {
                    watchlist_ptr->read_warning_timeout = 0; /* stop warning */
                }
            }
        }
    }

    Pthread_mutex_unlock(&(netinfo_ptr->watchlk));
}

void net_end_appsock(SBUF2 *sb)
{
    watchlist_node_type *watchlist_node;
    netinfo_type *netinfo_ptr;

    if (!sb)
        return;

    watchlist_node = get_watchlist_node(sb, __func__);
    if (watchlist_node) {
        netinfo_ptr = watchlist_node->netinfo_ptr;

        /* remove from the watch list, if it's on there */
        Pthread_mutex_lock(&(netinfo_ptr->watchlk));
        if (watchlist_node->in_watchlist) {
            listc_rfl(&(netinfo_ptr->watchlist), watchlist_node);
        }

        /* Restore original read/write functions so that if sbuf2close does a
         * flush it won't be trying to update the watchlist node. */
        sbuf2setrw(sb, watchlist_node->readfn, watchlist_node->writefn);

        free(watchlist_node);
        Pthread_mutex_unlock(&(netinfo_ptr->watchlk));
    }

    sbuf2close(sb);
}

void net_add_watch(SBUF2 *sb, int read_timeout, int write_timeout)
{
    watchlist_node_type *watchlist_node;
    netinfo_type *netinfo_ptr;

    watchlist_node = get_watchlist_node(sb, __func__);

    if (!watchlist_node)
        return;

    netinfo_ptr = watchlist_node->netinfo_ptr;

    Pthread_mutex_lock(&(netinfo_ptr->watchlk));

    if (watchlist_node->in_watchlist) {
        watchlist_node->read_timeout = read_timeout;
        watchlist_node->write_timeout = write_timeout;

        Pthread_mutex_unlock(&(netinfo_ptr->watchlk));

        return;
    }

    watchlist_node->read_timeout = read_timeout;
    watchlist_node->write_timeout = write_timeout;
    watchlist_node->in_watchlist = 1;

    listc_atl(&(netinfo_ptr->watchlist), watchlist_node);

    Pthread_mutex_unlock(&(netinfo_ptr->watchlk));
}

void net_set_writefn(SBUF2 *sb, sbuf2writefn writefn)
{
    watchlist_node_type *watchlist_node = sbuf2getuserptr(sb);
    if (watchlist_node == NULL)
        return;
    watchlist_node->writefn = writefn;
}

static void *heartbeat_check_thread(void *arg)
{
    host_node_type *ptr;
    netinfo_type *netinfo_ptr;
    int timestamp;
    int fd;
    int node_timestamp;
    int running_user_func;

    thread_started("net heartbeat check");

    netinfo_ptr = (netinfo_type *)arg;
    netinfo_ptr->heartbeat_check_thread_arch_tid = getarchtid();
    logmsg(LOGMSG_INFO, "heartbeat check thread starting.  time=%d.  tid=%d\n",
            netinfo_ptr->heartbeat_check_time,
            netinfo_ptr->heartbeat_check_thread_arch_tid);

    if (netinfo_ptr->start_thread_callback)
        netinfo_ptr->start_thread_callback(netinfo_ptr->callback_data);

    while (!netinfo_ptr->exiting) {
        int now;
        /* Re-register under portmux if it's time */
        if (netinfo_ptr->portmux_register_interval > 0 &&
            ((now = comdb2_time_epoch()) - netinfo_ptr->portmux_register_time) >
                netinfo_ptr->portmux_register_interval) {
            int pport;
            if (netinfo_ptr->port_from_lrl)
                pport = portmux_use(netinfo_ptr->app, netinfo_ptr->service,
                                    netinfo_ptr->instance, netinfo_ptr->myport);
            else
                pport = portmux_register(netinfo_ptr->app, netinfo_ptr->service,
                                         netinfo_ptr->instance);

            if (pport != netinfo_ptr->myport && pport > 0) {
                /* What on earth should i do?  Abort maybe?  i'm already using
                 * the old port, and sockpool has it cached everywhere .. */
                logmsg(LOGMSG_FATAL, "Portmux returned a different port for %s %s %s?  ",
                        netinfo_ptr->app, netinfo_ptr->service,
                        netinfo_ptr->instance);
                logmsg(LOGMSG_FATAL, "Oldport=%d, returned-port=%d\n",
                       netinfo_ptr->myport, pport);
                abort();
            }
            netinfo_ptr->portmux_register_time = now;
        }

        Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

        /*fprintf(stderr, "heartbeat thread running\n");*/

        for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
            if (ptr->host != netinfo_ptr->myhostname) {
                /* CLOSE it if we havent recieved a heartbeat from it */
                timestamp = time(NULL);

                Pthread_mutex_lock(&(ptr->timestamp_lock));
                fd = ptr->fd;
                running_user_func = ptr->running_user_func;
                node_timestamp = ptr->timestamp;
                Pthread_mutex_unlock(&(ptr->timestamp_lock));

                if ((fd > 0) && (running_user_func == 0)) {
                    if ((timestamp - node_timestamp) >
                        netinfo_ptr->heartbeat_check_time) {
                        host_node_printf(
                            LOGMSG_WARN,
                            ptr, "%s: no data in %d seconds, killing session\n",
                            __func__, timestamp - node_timestamp);

                        /* mark last failing subnet */
                        net_set_bad_subnet(ptr->subnet);

                        close_hostnode(ptr);
                    }
                }
            }
        }

        Pthread_rwlock_unlock(&(netinfo_ptr->lock));

        if (netinfo_ptr->exiting)
            break;
        sleep(1);
    }

    logmsg(LOGMSG_DEBUG, "heartbeat check thread exiting!\n");

    if (netinfo_ptr->stop_thread_callback)
        netinfo_ptr->stop_thread_callback(netinfo_ptr->callback_data);

    return NULL;
}

int net_close_connection(netinfo_type *netinfo_ptr, const char *hostname)
{
    host_node_type *ptr;
    int closed = 0;

    if (!isinterned(hostname))
        abort();

    logmsg(LOGMSG_USER, "Asked to close connection to %s on %s\n", hostname,
            netinfo_ptr->service);

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        if (ptr->host != hostname) {
            close_hostnode(ptr);
            closed = 1;
            break;
        }
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
    return !closed;
}

/* call this under netinfo_ptr lock ! */
static int is_ok(netinfo_type *netinfo_ptr, const char *host)
{
    host_node_type *host_node_ptr;
    if (netinfo_ptr->myhostname == host)
        return 1;
    for (host_node_ptr = netinfo_ptr->head; host_node_ptr != NULL;
         host_node_ptr = host_node_ptr->next) {
        if (host_node_ptr->host == host) {
            /* To prevent race conditions we should check this
             * stuff under lock. */
            int ok = 0;
            Pthread_mutex_lock(&(host_node_ptr->lock));
            if (host_node_ptr->fd > 0 && !host_node_ptr->decom_flag &&
                !host_node_ptr->closed && !host_node_ptr->really_closed) {
                ok = 1;
            }
            Pthread_mutex_unlock(&(host_node_ptr->lock));
            return ok;
        }
    }

    return 0;
}

int net_set_max_queue(netinfo_type *netinfo_ptr, int x)
{
    netinfo_ptr->max_queue = x;
    return 0;
}

int net_set_max_bytes(netinfo_type *netinfo_ptr, uint64_t x)
{
    netinfo_ptr->max_bytes = x;
    return 0;
}

int net_sanctioned_list_ok(netinfo_type *netinfo_ptr)
{
    sanc_node_type *sanc_node_ptr;
    int ok = 1;

    Pthread_mutex_lock(&(netinfo_ptr->sanclk));

    for (sanc_node_ptr = netinfo_ptr->sanctioned_list;
         ok && sanc_node_ptr != NULL; sanc_node_ptr = sanc_node_ptr->next) {
        ok = is_ok(netinfo_ptr, sanc_node_ptr->host);
    }

    Pthread_mutex_unlock(&(netinfo_ptr->sanclk));

    return ok;
}

static sanc_node_type *add_to_sanctioned_nolock(netinfo_type *netinfo_ptr,
                                                const char hostname[],
                                                int portnum)
{
    /* scan to see if it's already there */
    sanc_node_type *ptr = netinfo_ptr->sanctioned_list;

    while (ptr != NULL && ptr->host != hostname)
        ptr = ptr->next;

    if (ptr != NULL) {
        return ptr;
    }

    ptr = calloc(1, sizeof(sanc_node_type));
    ptr->next = netinfo_ptr->sanctioned_list;
    ptr->host = hostname;
    ptr->port = portnum;
    ptr->timestamp = time(NULL);

    netinfo_ptr->sanctioned_list = ptr;

    return ptr;
}

void connect_to_all(netinfo_type *netinfo_ptr)
{
    host_node_type *host_node_ptr;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (host_node_ptr = netinfo_ptr->head; host_node_ptr != NULL;
         host_node_ptr = host_node_ptr->next) {
        if (host_node_ptr->host != netinfo_ptr->myhostname)
            connect_to_host(netinfo_ptr, host_node_ptr, NULL);
        /*sleep(1);*/
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
}

/*
  1) set up a socket bound, and listening on our host/port
  2) create an accept_thread blocked on that socket
  3) create a connect thread for each entry in sites[] array
  4) create a heartbeat thread
*/

int net_init(netinfo_type *netinfo_ptr)
{
    int rc;
    host_node_type *host_node_ptr;

    /* block SIGPIPE */
    sighold(SIGPIPE);

    /* do nothing if we have a fake netinfo */
    if (netinfo_ptr->fake)
        return 0;

    /* add everything we have at this point to the sanctioned list */
    for (host_node_ptr = netinfo_ptr->head; host_node_ptr != NULL;
         host_node_ptr = host_node_ptr->next) {
        add_to_sanctioned_nolock(netinfo_ptr, host_node_ptr->host,
                                 host_node_ptr->port);
        host_node_printf(LOGMSG_INFO, host_node_ptr, "adding to sanctioned\n");
    }

    /* create heartbeat writer thread */
    rc = pthread_create(&(netinfo_ptr->heartbeat_send_thread_id),
                        &(netinfo_ptr->pthread_attr_detach),
                        heartbeat_send_thread, netinfo_ptr);
    if (rc != 0) {
        logmsg(LOGMSG_FATAL, "init_network:couldnt create heartbeat thread - "
                        "rc=%d errno=%d %s exiting\n",
                rc, errno, strerror(errno));
        exit(1);
    }

    /* create heartbeat reader thread */
    rc = pthread_create(&(netinfo_ptr->heartbeat_check_thread_id),
                        &(netinfo_ptr->pthread_attr_detach),
                        heartbeat_check_thread, netinfo_ptr);
    if (rc != 0) {
        logmsg(LOGMSG_FATAL, "init_network:couldnt create heartbeat thread - "
                        "rc=%d, errno=%d %s exiting\n",
                rc, errno, strerror(errno));
        exit(1);
    }

    if (netinfo_ptr->accept_on_child || !netinfo_ptr->ischild) {
        portmux_register_reconnect_callback(net_portmux_hello, netinfo_ptr);
        net_portmux_hello(netinfo_ptr);

        /* create accept thread */
        rc = pthread_create(&(netinfo_ptr->accept_thread_id),
                            &(netinfo_ptr->pthread_attr_detach), accept_thread,
                            netinfo_ptr);
        if (rc != 0) {
            logmsg(LOGMSG_FATAL, "init_network: couldnt create accept thread - "
                            "rc=%d, errno=%d %s exiting\n",
                    rc, errno, strerror(errno));
            exit(1);
        }

        /* dont proceed till the accept thread is set up */
        while (!netinfo_ptr->accept_thread_created) {
            usleep(10000);
        }
    }
    /*fprintf(stderr, "netinfo_ptr->accept_thread_created = %d\n",
       netinfo_ptr->accept_thread_created);*/

    /* create threads to connect to each host we know about */
    connect_to_all(netinfo_ptr);

    /* XXX just give things a chance to settle down before we return */
    usleep(10000);

    return 0;
}

static int net_portmux_hello(void *p)
{
    netinfo_type *netinfo_ptr = (netinfo_type *)p;
    if (netinfo_ptr->hellofd != -1) {
        close(netinfo_ptr->hellofd);
        netinfo_ptr->hellofd = -1;
    }
    char register_name[16 + 16 + MAX_DBNAME_LENGTH + 1];
    snprintf(register_name, sizeof(register_name), "%s/%s/%s", netinfo_ptr->app,
             netinfo_ptr->service, netinfo_ptr->instance);
    return portmux_hello("localhost", register_name, &netinfo_ptr->hellofd);
}

int net_register_admin_appsock(netinfo_type *netinfo_ptr, APPSOCKFP func)
{
    netinfo_ptr->admin_appsock_rtn = func;
    return 0;
}

int net_register_appsock(netinfo_type *netinfo_ptr, APPSOCKFP func)
{
    netinfo_ptr->appsock_rtn = func;
    return 0;
}

int net_register_allow(netinfo_type *netinfo_ptr, NETALLOWFP func)
{
    netinfo_ptr->allow_rtn = func;
    return 0;
}

int net_register_newnode(netinfo_type *netinfo_ptr, NEWNODEFP func)
{
    netinfo_ptr->new_node_rtn = func;
    return 0;
}

int net_set_pool_size(netinfo_type *netinfo_ptr, int size)
{
    int cnt = 0;
    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    for (host_node_type *ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next)
        cnt++;
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));
    if (cnt > 1) {
        logmsg(LOGMSG_ERROR, 
                "%s: cannot set pool size after open - pool_size is %d\n",
                __func__, netinfo_ptr->pool_size);
    } else {
        logmsg(LOGMSG_INFO, "%s: set pool size to %d\n", __func__, size);
        netinfo_ptr->pool_size = size;
    }
    return 0;
}

int net_set_heartbeat_send_time(netinfo_type *netinfo_ptr, int time)
{
    if (netinfo_ptr == NULL)
        return 0;
    netinfo_ptr->heartbeat_send_time = time;
    return 0;
}

int net_get_heartbeat_send_time(netinfo_type *netinfo_ptr)
{
    return netinfo_ptr->heartbeat_send_time;
}

int net_set_heartbeat_check_time(netinfo_type *netinfo_ptr, int time)
{
    if (netinfo_ptr == NULL)
        return 0;
    netinfo_ptr->heartbeat_check_time = time;
    return 0;
}

void net_set_callback_data(netinfo_type *info, void *data)
{
    info->callback_data = data;
}

void net_register_start_thread_callback(netinfo_type *info,
                                        void (*callback)(void *))
{
    info->start_thread_callback = callback;
}

void net_register_stop_thread_callback(netinfo_type *info,
                                       void (*callback)(void *))
{
    info->stop_thread_callback = callback;
}

static char *net_get_osql_node_ll(netinfo_type *netinfo_ptr,
                                  char *blk_nodes[REPMAX], int n_blknodes)
{
    host_node_type *ptr, *nodes[REPMAX]; /* 16 siblings, more than reasonable
                                          * replicated cluster */
    int nnodes = 0;
    int index = 0;
    int ix = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        /* prefering to offload */
        if (ptr->host == netinfo_ptr->myhostname)
            continue;

        /* disconnected already ?*/
        if (ptr->fd <= 0 || !ptr->got_hello)
            continue;

        /* is rtcpu-ed? */
        if (machine_is_up(ptr->host) != 1)
            continue;

        /* is blackout ? */
        for (ix = 0; ix < n_blknodes; ix++) {
            if (blk_nodes[ix] != ptr->host)
                break;
        }

        /* blacklist is actually whitelist */
        if (n_blknodes && ix >= n_blknodes)
            // didn't find node in white-list
            continue;

        if (nnodes >= REPMAX)
            break;

        nodes[nnodes++] = ptr;
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    /* no siblings */
    if (!nnodes)
        return NULL;

    {
        /* avoid sending all same sec requests to the same node */
        static int init = 0;
        if (!init) {
            unsigned int t = time(NULL);
            index = init = rand_r(&t) % nnodes; /* let it spread */
        } else {
            init = (init + 1) % nnodes; /* round robin */
            index = init;
        }
    }

    return nodes[index]->host;
}

/* pick a sibling for sql offloading */
char *net_get_osql_node(netinfo_type *netinfo_ptr)
{
    return net_get_osql_node_ll(netinfo_ptr, NULL, 0);
}

/*
   pick a sibling for sql offloading using blackout list
   UPDATE: meaning change, the blkout list will contain valid nodes!
 */
char *net_get_osql_node_blkout(netinfo_type *netinfo_ptr, char *nodes[REPMAX],
                               int lnodes)
{
    return net_get_osql_node_ll(netinfo_ptr, nodes, lnodes);
}

char *net_get_mynode(netinfo_type *netinfo_ptr)
{
    return netinfo_ptr->myhostname;
}

void *net_get_usrptr(netinfo_type *netinfo_ptr) { return netinfo_ptr->usrptr; }

void net_set_usrptr(netinfo_type *netinfo_ptr, void *usrptr)
{
    netinfo_ptr->usrptr = usrptr;
}

/* deprecated */
void net_sleep_with_lock(netinfo_type *netinfo_ptr, int nseconds)
{
    logmsg(LOGMSG_ERROR, "%s is deprecated\n", __func__);
}

int net_get_nodes_info(netinfo_type *netinfo_ptr, int max_nodes,
                       struct host_node_info *out_nodes)
{
    host_node_type *ptr;
    int count = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));

    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        if (max_nodes > 0) {
            out_nodes->fd = ptr->fd;
            out_nodes->host = ptr->host;
            out_nodes->port = ptr->port;

            out_nodes++;
            max_nodes--;
        }
        count++;
    }

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return count;
}

int net_get_host_network_usage(netinfo_type *netinfo_ptr, const char *host,
                               unsigned long long *written,
                               unsigned long long *read,
                               unsigned long long *throttle_waits,
                               unsigned long long *reorders)
{
    host_node_type *ptr;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        if (ptr->host == host)
            break;
    }
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    if (ptr == NULL)
        return -1;

    *written = ptr->stats.bytes_written;
    *read = ptr->stats.bytes_read;
    *throttle_waits = ptr->stats.throttle_waits;
    *reorders = ptr->stats.reorders;

    return 0;
}

int net_get_network_usage(netinfo_type *netinfo_ptr,
                          unsigned long long *written, unsigned long long *read,
                          unsigned long long *throttle_waits,
                          unsigned long long *reorders)
{
    *written = netinfo_ptr->stats.bytes_written;
    *read = netinfo_ptr->stats.bytes_read;
    *throttle_waits = netinfo_ptr->stats.throttle_waits;
    *reorders = netinfo_ptr->stats.reorders;
    return 0;
}

int net_get_my_port(netinfo_type *netinfo_ptr) { return netinfo_ptr->myport; }

void net_trace(netinfo_type *netinfo_ptr, int on) { netinfo_ptr->trace = on; }

void net_enable_test(netinfo_type *netinfo_ptr, int test)
{
    netinfo_ptr->net_test = test;
}

void net_disable_test(netinfo_type *netinfo_ptr) { netinfo_ptr->net_test = 0; }

void net_set_poll(netinfo_type *netinfo_ptr, int polltm)
{
    netinfo_ptr->netpoll = polltm;
}

int net_get_poll(netinfo_type *netinfo_ptr) { return netinfo_ptr->netpoll; }

int get_host_port(netinfo_type *netinfo)
{
    Pthread_rwlock_rdlock(&(netinfo->lock));
    int port = netinfo->myport;
    Pthread_rwlock_unlock(&(netinfo->lock));
    return port;
}

void net_add_watch_warning(SBUF2 *sb, int read_warning_timeout,
                           int write_timeout, void *arg,
                           int (*callback)(void *, int, int))
{
    watchlist_node_type *watchlist_node;
    netinfo_type *netinfo_ptr;

    watchlist_node = get_watchlist_node(sb, __func__);

    if (!watchlist_node)
        return;

    netinfo_ptr = watchlist_node->netinfo_ptr;

    Pthread_mutex_lock(&(netinfo_ptr->watchlk));

    if (watchlist_node->in_watchlist) {
        watchlist_node->read_timeout = 0;
        watchlist_node->write_timeout = write_timeout;
        watchlist_node->read_warning_timeout = read_warning_timeout;
        watchlist_node->read_warning_arg = arg;
        watchlist_node->read_warning_func = callback;

        Pthread_mutex_unlock(&(netinfo_ptr->watchlk));

        return;
    }

    watchlist_node->in_watchlist = 1;
    watchlist_node->read_timeout = 0;
    watchlist_node->write_timeout = write_timeout;
    watchlist_node->read_warning_timeout = read_warning_timeout;
    watchlist_node->read_warning_arg = arg;
    watchlist_node->read_warning_func = callback;

    listc_atl(&(netinfo_ptr->watchlist), watchlist_node);

    Pthread_mutex_unlock(&(netinfo_ptr->watchlk));
}

int net_appsock_get_addr(SBUF2 *sb, struct sockaddr_in *addr)
{
    watchlist_node_type *watchlist_node;

    watchlist_node = get_watchlist_node(sb, __func__);
    if (!watchlist_node)
        return 1;

    *addr = watchlist_node->addr;
    return 0;
}

/* return a socket set up for listening on given port */
int net_listen(int port)
{
    struct sockaddr_in sin;
    int listenfd;
    int tcpbfsz;
    int reuse_addr;
    struct linger linger_data;
    int keep_alive;
    int flag;
    int rc;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(port);

    /* TODO: make these tunable */

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        logmsg(LOGMSG_ERROR, "%s: socket rc %d %s\n", __func__, errno,
                strerror(errno));
        return -1;
    }

#ifdef NODELAY
    flag = 1;
    rc = setsockopt(listenfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                    sizeof(int));
    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "%s: couldnt turn off nagel on listenfd %d: %d %s\n",
                __func__, listenfd, errno, strerror(errno));
        return -1;
    }
#endif

#ifdef TCPBUFSZ
    tcpbfsz = (8 * 1024 * 1024);
    rc = setsockopt(listenfd, SOL_SOCKET, SO_SNDBUF, &tcpbfsz, sizeof(tcpbfsz));
    if (rc < 0) {
        logmsg(LOGMSG_ERROR, 
                "%s: couldnt set tcp sndbuf size on listenfd %d: %d %s\n",
                __func__, listenfd, errno, strerror(errno));
        return -1;
    }

    tcpbfsz = (8 * 1024 * 1024);
    rc = setsockopt(listenfd, SOL_SOCKET, SO_RCVBUF, &tcpbfsz, sizeof(tcpbfsz));
    if (rc < 0) {
        logmsg(LOGMSG_ERROR, 
                "%s: couldnt set tcp rcvbuf size on listenfd %d: %d %s\n",
                __func__, listenfd, errno, strerror(errno));
        return -1;
    }
#endif

    /* allow reuse of local addresses */
    reuse_addr = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse_addr,
                   sizeof(reuse_addr)) != 0) {
        logmsg(LOGMSG_ERROR, "%s: coun't set reuseaddr %d %s\n", __func__, errno,
                strerror(errno));
        return -1;
    }

#ifdef NOLINGER
    linger_data.l_onoff = 0;
    linger_data.l_linger = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_LINGER, (char *)&linger_data,
                   sizeof(linger_data)) != 0) {
        logmsg(LOGMSG_ERROR, "%s: coun't set keepalive %d %s\n", __func__, errno,
                strerror(errno));
        return -1;
    }
#endif

    /* enable keepalive timer. */
    keep_alive = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_KEEPALIVE, (char *)&keep_alive,
                   sizeof(keep_alive)) != 0) {
        logmsg(LOGMSG_ERROR, "%s: coun't set keepalive %d %s\n", __func__, errno,
                strerror(errno));
        return -1;
    }

    /* bind an address to the socket */
    if (bind(listenfd, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
        logmsg(LOGMSG_ERROR, "%s: FAILED TO BIND to port %d: %d %s\n", __func__,
                port, errno, strerror(errno));
        return -1;
    }

    /* listen for connections on socket */
    if (listen(listenfd, SOMAXCONN) < 0) {
        logmsg(LOGMSG_ERROR, "%s: listen rc %d %s\n", __func__, errno,
                strerror(errno));
        return -1;
    }

    return listenfd;
}

void net_set_conntime_dump_period(netinfo_type *netinfo_ptr, int value)  {
    netinfo_ptr->conntime_dump_period = value;
}

int net_get_conntime_dump_period(netinfo_type *netinfo_ptr) {
    return netinfo_ptr->conntime_dump_period;
}

int net_get_stats(netinfo_type *netinfo_ptr, struct net_stats *stat) {
    struct host_node_tag *ptr;

    stat->num_drops = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next)
        stat->num_drops = ptr->num_queue_full;

    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return 0;
}

int net_get_host_stats(netinfo_type *netinfo_ptr, const char *host, struct net_host_stats *stat) {
    struct host_node_tag *ptr;
    stat->queue_size = 0;

    Pthread_rwlock_rdlock(&(netinfo_ptr->lock));
    for (ptr = netinfo_ptr->head; ptr != NULL; ptr = ptr->next) {
        if (strcmp(host, ptr->host) == 0) {
            stat->queue_size = time_metric_max(ptr->metric_queue_size);
            break;
        }
    }
    Pthread_rwlock_unlock(&(netinfo_ptr->lock));

    return 0;
}
