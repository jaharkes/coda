/* BLURB lgpl

                           Coda File System
                              Release 6

          Copyright (c) 2017-2018 Carnegie Mellon University
                  Additional copyrights listed below

This  code  is  distributed "AS IS" without warranty of any kind under
the  terms of the  GNU  Library General Public Licence  Version 2,  as
shown in the file LICENSE. The technical and financial contributors to
Coda are listed in the file CREDITS.

                        Additional copyrights

#*/

/*
   Daemon that does the relaying of packets to/from net and localhost.
   Created via fork() by Venus or codasrv.

   Uses single Unix domain socket to talk to Venus or codasrv on
   localhost, and one TCP-tunneled socket to talk to each distinct
   remote Coda server or client.  Also has one UDP socket for backward
   compatibility with legacy servers and clients.

   This code layers UDP socket primitives on top of TCP connections.
   Maintains a single TCP connection for each (host, port) pair
   All UDP packets to/from that (host, port) pair are sent/recvd on this connection.
   All RPC2 connections to/from that (host,port are multiplexed on this connection.
   Minimal changes to rest of the RPC2 code.
   Discards all packets with "RETRY" bit set.

   Possible negative consequences:
   (a) serializes all transmissions to each (host,port) pair
       (but no guarantee that such serialization wasn't happening before)
   (b) SFTP becomes a stop and wait protocol for each 8-packet window
       (since RETRY flag triggered sendahead)

   (Satya, 2017-01-04)
*/

/* Encapsulation rules: Is ctp_t packet present as prefix to UDP packet?
   (1) Venus/CodaSrv to/from codatunnel daemon:  yes; ctp_t fields in host order
   (2) codatunnel daemon to/from network via udpsocket:  no
   (3) codatunnel daemon to/from network via tcpsocket: yes; ctp_t fields in network order
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <uv.h>

#include "codatunnel.private.h"

/* Global variables within codatunnel daemon */
static int codatunnel_I_am_server = 0; /* only clients initiate; only servers accept */
static int codatunnel_onlytcp = 0;    /* whether to use UDP fallback; default is yes */

static uv_loop_t *codatunnel_main_loop = 0;
static uv_udp_t codatunnel;  /* facing Venus or CodaSrv */
static uv_udp_t udpsocket;   /* facing the network */
static uv_tcp_t tcpbindsocket;   /* facing the network */

/* Useful data structures for minicbs; these minicbs do little real work
   and are mainly used to free malloc'ed data structures after transmission;
   original naming scheme was confusing, so these are simply numbered 1, 2, 3, etc.*/
typedef struct {
  uv_udp_send_t req;
  uv_buf_t msg[2];
  ctp_t ctp;
} minicb1_req_t;  /* used to be udp_send_req_t */

typedef struct {
  uv_write_t req;
  uv_buf_t msg;
  dest_t *dest;
} minicb2_req_t;  /* used to be tcp_send_req_t */

typedef struct {
  uv_udp_send_t req;
  uv_buf_t msg;
  dest_t *dest;
} minicb3_req_t;


/* forward refs for workhorse functions; many are cb functions */
static void recv_codatunnel_cb(uv_udp_t *, ssize_t, const uv_buf_t *,
                               const struct sockaddr *, unsigned);
static void send_to_udp_dest(dest_t *, uv_udp_t *, ssize_t, const uv_buf_t *,
                               const struct sockaddr *, unsigned);
static void send_to_tcp_dest(dest_t *, ssize_t, const uv_buf_t *);
static void try_creating_tcp_connection (dest_t *);
static void recv_tcp_cb (uv_stream_t *, ssize_t, const uv_buf_t *);
static void tcp_connect_cb(uv_connect_t *, int);
static void recv_udpsocket_cb(uv_udp_t *, ssize_t, const uv_buf_t *,
                              const struct sockaddr *, unsigned);
static void tcp_newconnection_cb(uv_stream_t *, int);


static socklen_t sockaddr_len(const struct sockaddr *addr)
{
    if (addr->sa_family == AF_INET)
        return sizeof(struct sockaddr_in);

    if (addr->sa_family == AF_INET6)
        return sizeof(struct sockaddr_in6);

    return 0;
}


static void alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    *buf = uv_buf_init(malloc(suggested_size), suggested_size);

    /* gracefully handle allocation failures on libuv < 1.10.0 */
    if (buf->base == NULL) buf->len = 0;
}


/* All the minicb()s are gathered here in one place */

static void minicb1(uv_udp_send_t *arg, int status)
/* used to be udp_sent_cb() */
{
    /*  DEBUG("minicb1(%p, %d)\n", arg, status); */
    minicb1_req_t *req = (minicb1_req_t *)arg;
    free(req->msg[1].base);
    free(req);
}

static void minicb2(uv_write_t *arg, int status)
/* used to tcp_send_req() */
{
    minicb2_req_t *req = (minicb2_req_t *) arg;
    dest_t *d = req->dest;

    DEBUG("minicb2(%p, %d)\n", arg, status);

    if (status != 0) {
        DEBUG("tcp connection error: %s after %d packets\n",
              uv_strerror(status), d->packets_sent);
        d->state = TCPBROKEN;
        free(d->tcphandle);
        d->tcphandle = 0;
    }
    else {
        d->packets_sent++;   /* one more was sent out! */
    }

    free(req->msg.base);
    free(req);
}

static void minicb3(uv_udp_send_t *arg, int status)
{
    minicb3_req_t *req = (minicb3_req_t *) arg;
    DEBUG("minicb3(%p, %d)\n", arg, status);

    free(req->msg.base);
    free(req);
}


static void recv_codatunnel_cb(uv_udp_t *codatunnel, ssize_t nread,
                               const uv_buf_t *buf,
                               const struct sockaddr *addr,
                               unsigned flags)
{
    static unsigned empties;

    DEBUG("packet received from codatunnel nread=%ld buf=%p addr=%p flags=%u\n",
          nread, buf ? buf->base : NULL, addr, flags);

    if (nread == UV_ENOBUFS)
        return;

    if (nread == 0) {
        /* empty packet received, we normally get this after we've drained any
         * pending data from the socket after a wakeup. But we also see these
         * when the other end of a socketpair was closed. Differentiate by
         * counting how many successive empties we get. --JH */
        if (++empties >= 3) {
            DEBUG("codatunnel closed\n");
            uv_stop(codatunnel_main_loop);
            uv_close((uv_handle_t *)codatunnel, NULL);
        }
        free(buf->base);
        return;
    }
    empties = 0;

    if (nread < 0) {
        /* We shouldn't see read errors on the codatunnel socketpair. -JH */
        /* if we close the socketpair endpoint, we might just as well stop */
        uv_stop(codatunnel_main_loop);
        uv_close((uv_handle_t *)codatunnel, NULL);
        free(buf->base);
        return;
    }
    if (nread < sizeof(ctp_t)) {
        DEBUG("short packet received from codatunnel\n");
        free(buf->base);
        return;
    }

    /* We have a legit packet; it was already been read into buf
       before this upcall was invoked by libuv */

    ctp_t *p = (ctp_t *)buf->base;

    dest_t *d = getdest(&p->addr, p->addrlen);

    if (!d) /* new destination */
        d = createdest(&p->addr, p->addrlen);

    /* what do we do with packet p for destination d? */

    if (d->state == TCPACTIVE) {
        DEBUG("d->state == TCPACTIVE\n");

        if (p->is_retry && (d->packets_sent > 0)) {
            /* drop retry packet;
               only exception is when nothing has yet been sent on new TCP
               connection; the state may have become TCPACTIVE after most
               recent retry;  those earlier retries via UDP may all have
               been lost (e.g., because of firewall settings at dest);
               make sure you send at least this one; all future
               retries will be dropped;  no harm if earlier retries got through
               (Satya, 1/20/2018)
            */
            free(buf->base); /* do this now since no cascaded cb */
        }
        else send_to_tcp_dest(d, nread, buf); /* free buf only in cascaded cb */
        return;
    }

    /*
       UDP fallback: always forward UDP packets if TCPBROKEN; RPC2
       duplicate elimination at higher level will drop as needed for
       at-most-once semantics; if TCPACTIVE happens later for this
       destination, early packets will be sent by UDP, but later ones
       by TCP; nothing special needs to be done to track these or
       avoid race conditions; the higher level processing in RPC2 will
       ensure at-most-once semantics regardless of how the packet
       traveled (Satya, 1/20/2018)
    */
    if (!codatunnel_onlytcp)
        send_to_udp_dest(d, codatunnel, nread, buf, addr, flags); /* free buf only in cascaded cb */

    /* Try to establish a new TCP connection for future use;
       do this only once per INIT1 (avoiding retries) to avoid TCP SYN flood;
       Only clients should attempt this, because of NAT firewalls */
    if (p->is_init1  && (!p->is_retry) && (!codatunnel_I_am_server)) {
        try_creating_tcp_connection(d);
    }
}


static void send_to_udp_dest(dest_t *d, uv_udp_t *codatunnel, ssize_t nread,
                               const uv_buf_t *buf,
                               const struct sockaddr *addr,
                               unsigned flags)
{
    /* Somewhat complicated structure is to avoid data copy of payload */

    ctp_t *p = (ctp_t *)buf->base;
    minicb1_req_t *req;

    req = malloc(sizeof(minicb1_req_t));
    /* data to send is what follows the codatunnel packet header */
    req->msg[0] = uv_buf_init(buf->base + sizeof(ctp_t),
                              nread - sizeof(ctp_t));

    /* move buffer from reader to writer, we won't actually send msg[1] but
     * this way the buffer will get properly freed in `minicb1` */
    req->msg[1] = uv_buf_init(buf->base, nread);

    /* forward packet to the remote host */
    uv_udp_send((uv_udp_send_t *)req, &udpsocket, req->msg, 1,
                (struct sockaddr *)&p->addr, minicb1);
}


static void send_to_tcp_dest(dest_t *d, ssize_t nread, const uv_buf_t *buf)
{
    minicb2_req_t *req; /* req can't be local because of use in callback */

    DEBUG("send_to_tcp_dest(%p, %d, %p)\n", d, nread, buf);

    req = malloc(sizeof(minicb2_req_t));
    req->dest = d;
    req->msg = uv_buf_init(buf->base, nread); /* no stripping; send entire codatunnel packet */

    /* Convert ctp_d fields to network order */
    ctp_t *p = (ctp_t *)buf->base;
    DEBUG("is_retry = %u is_init1 = %u  msglen = %lu\n",
          p->is_retry, p->is_init1, p->msglen);

    p->is_retry = htonl(p->is_retry);
    p->is_init1 = htonl(p->is_init1);
    p->msglen = htonl(p->msglen);
    /* ignoring addrlen for now; perhaps already in network order? */


    /* forward packet to the remote host */
    DEBUG("Going to do uv_write(%p, %p, %p,...)\n",
          req, d->tcphandle,  &req->msg);
    int rc = uv_write((uv_write_t *)req, (uv_stream_t *)d->tcphandle,  &req->msg, 1, minicb2);
    DEBUG("uv_write(): rc = %d\n", rc);
}

static void tcp_connect_cb(uv_connect_t *req, int status)
{
    DEBUG("tcp_connect_cb(%p, %d)\n", req, status);
    dest_t *d = req->data;

    if (status == 0) {/* connection successful */
        DEBUG("tcp_connect_cb(%p, %d) --> %p\n", d, status, d->tcphandle);
        (d->tcphandle)->data = d; /* point back, for use in upcalls */
        d->received_packet = malloc(MAXRECEIVE);  /* freed in uv_udp_sent_cb() */
        memset(d->received_packet, 0, MAXRECEIVE);
        d->nextbyte = 0;
        d->packets_sent = 0;
        d->ntoh_done = 0;
        d->state = TCPACTIVE; /* commit point */

        int rc = uv_read_start((uv_stream_t *)d->tcphandle, alloc_cb, recv_tcp_cb);
        DEBUG("uv_read_start() --> %d\n", rc);
    }
    /* else connection attempt failed: do nothing */

    free(req);
}


static void  try_creating_tcp_connection(dest_t *d)
{
    uv_connect_t *req;

    DEBUG("try_creating_tcp_connection(%p)\n", d);
    d->tcphandle = malloc(sizeof(uv_tcp_t));
    uv_tcp_init(codatunnel_main_loop, d->tcphandle);
    req = malloc(sizeof(uv_connect_t));
    req->data = d;  /* so we can identify dest in upcall */
    int rc = uv_tcp_connect(req, d->tcphandle, (struct sockaddr *)(&d->destaddr),
                            tcp_connect_cb);
    DEBUG("uv_tcp_connect --> %d\n", rc);
}


static void recv_tcp_cb (uv_stream_t *tcphandle, ssize_t nread, const uv_buf_t *buf)
{
    DEBUG("recv_tcp_cb (%p, %d, %p)\n", tcphandle, (int) nread, buf);

    dest_t *d = tcphandle->data;
    DEBUG("d = %p\n", d);
    DEBUG("d->nextbyte = %d  d->ntoh_done = %d\n", d->nextbyte, d->ntoh_done);

    if (nread < 0) {
        DEBUG("recv_tcp_cb() --> %s\n", uv_strerror(nread));
        d->state = TCPBROKEN;
        uv_close((uv_handle_t *)d->tcphandle, NULL);
        free(buf->base);
        free(d->tcphandle);
        d->tcphandle = 0;
        return;
    }

    if (nread == 0) {
        /* similar to EAGAIN or EWOULDBLOCK according to libuv manual */
        DEBUG("recv_tcp_cb() --> no-op\n");
        free(buf->base);
        return;
    }

    /* else nread > 0: many possible cases

       Case 1: (best case) nread corresponds to exactly one full packet
       to handoff; life is good!

       Case 2: nread is just few bytes, and not even enough to complete
       the codatunnel packet header; we have to wait to complete it in
       one or more future reads, then discover UDP packet len, and thus
       discover how many more bytes we need; may take many future upcalls

       Case 3: nread is a huge number of bytes;  many back-to-back UDP
       packets have arrived bunched together as one large read upcall;
       many UDP packets peeled off from this one upcall

       Case 4, 5, 6 ...:  variants of above

       Regardless of case, the requirement is that all nread bytes
       be consumed in this upcall; any future work has to be set up
       in the dest_t data structure.
    */
    DEBUG("buf->base = %p    buf->len = %lu\n", buf->base, buf->len);
    hexdump ("buf->base", buf->base, 64);  /* just print first 64 bytes */

    int bytesleft = nread;
    DEBUG("bytesleft = %d\n", bytesleft);
    do { /* each iteration does one memcpy() from src to tgt */
        DEBUG("bytesleft = %d  nextbyte = %d  ntoh_done = %d\n",
                bytesleft, d->nextbyte, d->ntoh_done);
        char *tgt = &((d->received_packet)[d->nextbyte]);
        char *src = (char *) &((buf->base)[nread-bytesleft]);
        int needed;

        if (d->nextbyte < sizeof(ctp_t)) {
            /* CASE 1: we don't even have a full ctp_t yet */
            needed = sizeof(ctp_t) - d->nextbyte;

            if (needed <= bytesleft) {
                memcpy(tgt, src, needed);
                d->nextbyte += needed;
                bytesleft -= needed;
            }
            else {
                memcpy(tgt, src, bytesleft);
                d->nextbyte += bytesleft;
                bytesleft = 0; /* all consumed and need more */
            }
            continue; /* outer do{} loop; */
        }

        /* CASE 2 (else part): If we reach here, we have
           a complete ctp_t at the head of d->received_packet */

        ctp_t *p = (ctp_t *) d->received_packet;

        /* Convert fields to host order; but only do it once; we may
           encounter this ctp_t many times if a big packet is fragmented into
           tiny pieces and we have to do many tcp_recv_cb() operations to
           reassemble it
        */
        if (d->ntoh_done == 0) {
            p->is_retry = ntohl(p->is_retry);
            p->is_init1 = ntohl(p->is_init1);
            p->msglen = ntohl(p->msglen);
            /* ignoring p->addrlen for now; perhaps already in network order? */
            d->ntoh_done = 1;
        }

        DEBUG("is_retry = %u  is_init1 = %u  msglen = %lu\n",
              p->is_retry, p->is_init1, p->msglen);

        if (p->msglen > (MAXRECEIVE - sizeof(ctp_t))) {
            /* we can't handle this monster */
            DEBUG("Monster packet of size %lu, giving up\n", p->msglen);
            d->state = TCPBROKEN;
            uv_close((uv_handle_t *)d->tcphandle, NULL);
            free(buf->base);
            free(d->tcphandle);
            d->tcphandle = 0;
            return;
        }

        needed = p->msglen - (d->nextbyte - sizeof(ctp_t));
        DEBUG("needed = %d\n", needed);

        if (needed <= bytesleft) {
            memcpy(tgt, src, needed);
            d->nextbyte += needed;
            bytesleft -= needed;
        }
        else {
            memcpy(tgt, src, bytesleft);
            d->nextbyte += bytesleft;
            bytesleft = 0; /* all consumed and need more */
        }

        /* Check whether we have a complete packet to handoff now */
        needed = p->msglen -(d->nextbyte - sizeof(ctp_t));
        DEBUG("Complete packet check, needed = %d\n", needed);

        if (needed > 0) continue; /* not complete yet */

        /* Victory: we have assembled a complete packet to handoff to
           venus or codasrv; no stripping of ctp_t needed */
        DEBUG("Full packet assembled, handing off\n");

        /* Replace recipient address with sender's address, so that
           recvfrom() can provide the "from" address.
           I assume that addrlen doesn't change; should we check this? */
        memcpy (&p->addr, &d->destaddr, sizeof(struct sockaddr_storage));

        minicb3_req_t *req =  (minicb3_req_t *) malloc(sizeof(minicb3_req_t));
        req->msg = uv_buf_init(d->received_packet, (sizeof(ctp_t) + p->msglen)); /* to send */
        /* forward packet to venus/codasrv; note that d->destaddr in
           uv_udp_send() below is really a "fake addr"; this is the source
           UDP addr rather than dest of this UDP packet; but codatunnel
           and venus/codasrv all ignore this so just use this as dummy value */

        uv_udp_send((uv_udp_send_t *)req, &codatunnel, &req->msg, 1,
                (struct sockaddr *)&d->destaddr, minicb3);

        /* Now prepare for read of a new packet */
        d->received_packet = malloc(MAXRECEIVE);
        memset(d->received_packet, 0, MAXRECEIVE);
        d->nextbyte = 0;
        d->ntoh_done = 0;

    } while (bytesleft > 0);

    /* We have now consumed all the bytes associated with this upcall */
    free(buf->base);
}



static void recv_udpsocket_cb(uv_udp_t *udpsocket, ssize_t nread,
                              const uv_buf_t *buf,
                              const struct sockaddr *addr,
                              unsigned flags)
{
    minicb1_req_t *req;
    struct sockaddr_in peer = {
        .sin_family = AF_INET,
    };

    DEBUG("packet received from udpsocket nread=%ld buf=%p addr=%p flags=%u\n",
          nread, buf ? buf->base : NULL, addr, flags);

    if (nread == UV_ENOBUFS)
        return;

    if (nread < 0) {
        /* I believe recoverable errors should be handled by libuv. -JH */
        /* if we close the udp listen socket, we might just as well stop */
        uv_stop(codatunnel_main_loop);
        uv_close((uv_handle_t *)udpsocket, NULL);
        free(buf->base);
        return;
    }

    if (nread == 0) {
        free(buf->base);
        return;
    }

    req = malloc(sizeof(minicb1_req_t));
    req->msg[0] = uv_buf_init((char *)&req->ctp, sizeof(ctp_t));
    req->ctp.addrlen = sockaddr_len(addr);
    memcpy(&req->ctp.addr, addr, req->ctp.addrlen);
    req->ctp.msglen = nread;
    strncpy(req->ctp.magic, "magic01", 8);

    /* move buffer from reader to writer */
    req->msg[1] = uv_buf_init(buf->base, nread);

    /* forward packet to venus/codasrv */
    uv_udp_send((uv_udp_send_t *)req, &codatunnel, req->msg, 2,
                (struct sockaddr *)&peer, minicb1);
}

static void tcp_newconnection_cb (uv_stream_t *bindhandle, int status)
{
    uv_tcp_t *clienthandle;
    struct sockaddr_storage peeraddr;
    int peerlen, rc;
    dest_t *d;

    DEBUG("bindhandle = %p, status = %d)\n", bindhandle, status);
    if (status != 0) {
        DEBUG("tcp_newconnection_cb() --> %s\n", uv_strerror(status));
        return;
    }

    /* clienthandle can't be local because its lifetime extends
       beyond this call; I haven't carefully thought through any possible
       memory leaks due to this malloc (Satya, 3/22/2018) */
    clienthandle = (uv_tcp_t *)malloc(sizeof(uv_tcp_t));

    uv_tcp_init(codatunnel_main_loop, clienthandle);
    rc = uv_accept(bindhandle, (uv_stream_t *)clienthandle);
    DEBUG("uv_accept() --> %d\n", rc);
    if (rc < 0) {
        DEBUG("uv_accept() --> %s\n", uv_strerror(rc));
        return;
    }

    /* Figure out identity of new client and create dest structure */
    peerlen = sizeof(peeraddr);
    rc = uv_tcp_getpeername(clienthandle, (struct sockaddr *)&peeraddr, &peerlen);
    DEBUG("uv_tcp_getpeername() --> %d\n", rc);
    if (rc < 0) {
        DEBUG("uv_tcp_getpeername() --> %s\n", uv_strerror(rc));
        return;
    }

    d = getdest(&peeraddr, sizeof(peeraddr));
    if (!d) {/* new destination */
        d = createdest(&peeraddr, sizeof(peeraddr));
    }

    /* Bind this TCP handle and dest */
    clienthandle->data = d;

    d->tcphandle = clienthandle;
    d->received_packet = malloc(MAXRECEIVE);
    /* all other fields of *d set by cleardest() in createdest() */
    d->state = TCPACTIVE; /* commit point */

    /* now start receiving data on this TCP connection */
    DEBUG("About to call uv_read_start()\n");
    rc = uv_read_start((uv_stream_t *)d->tcphandle, alloc_cb, recv_tcp_cb);
    DEBUG("uv_read_start() --> %d\n", rc);
}

/* main routine of coda tunnel daemon */
void codatunneld(int codatunnel_sockfd,
                 const char *tcp_bindaddr,
                 const char *udp_bindaddr,
                 const char *bind_service,
                 int onlytcp)
{
    uv_getaddrinfo_t gai_req;
    const struct addrinfo *ai, gai_hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_DGRAM,
        .ai_flags = AI_PASSIVE,
    };
    int rc;

    fprintf(stderr, "codatunneld: starting\n");

    if (tcp_bindaddr) codatunnel_I_am_server = 1; /* remember who I am */

    if (onlytcp) codatunnel_onlytcp = 1; /* no UDP fallback */

    /* make sure that writing to closed pipes doesn't kill us */
    signal(SIGPIPE, SIG_IGN);

    codatunnel_main_loop = uv_default_loop();

    /* bind codatunnel_sockfd */
    uv_udp_init(codatunnel_main_loop, &codatunnel);
    uv_udp_open(&codatunnel, codatunnel_sockfd);

    /* resolve the requested udp bind address */
    const char *node = (udp_bindaddr && *udp_bindaddr) ? udp_bindaddr : NULL;
    const char *service = bind_service ? bind_service : "0";
    rc = uv_getaddrinfo(codatunnel_main_loop, &gai_req, NULL, node,
			service, &gai_hints);
    if (rc < 0) {
        ERROR("uv_getaddrinfo() --> %s\n", uv_strerror(rc));
        exit(-1);
    }

    /* try to bind to any of the resolved addresses */
    uv_udp_init(codatunnel_main_loop, &udpsocket);
    for (ai = gai_req.addrinfo; ai != NULL; ai = ai->ai_next) {
        if (uv_udp_bind(&udpsocket, ai->ai_addr, 0) == 0)
            break;
    }
    if (!ai) {
        ERROR("uv_udp_bind() unsuccessful, exiting\n");
        exit(-1);
    }
    else uv_freeaddrinfo(gai_req.addrinfo);

    uv_udp_recv_start(&codatunnel, alloc_cb, recv_codatunnel_cb);
    uv_udp_recv_start(&udpsocket, alloc_cb, recv_udpsocket_cb);

    if (codatunnel_I_am_server) {
        /* start listening for connect() attempts */
        const struct addrinfo gai_hints2 = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
            .ai_flags = AI_PASSIVE,
        };
        /* service was already set earlier */


        uv_tcp_init(codatunnel_main_loop, &tcpbindsocket);

        /* try to bind to any of the resolved addresses */
        uv_getaddrinfo(codatunnel_main_loop, &gai_req, NULL, tcp_bindaddr,
                service, &gai_hints2);
        for (ai = gai_req.addrinfo; ai != NULL; ai = ai->ai_next) {
            if (uv_tcp_bind(&tcpbindsocket, ai->ai_addr, 0) == 0)
                break;
        }
        if (!ai) {
            ERROR("uv_tcp_bind() unsuccessful, exiting\n");
            exit(-1);
        }
        else uv_freeaddrinfo(gai_req.addrinfo);

        /* start listening for connect() attempts */
        uv_listen((uv_stream_t *)&tcpbindsocket, 10, tcp_newconnection_cb);
    }

    /* run until the codatunnel connection closes */
    uv_run(codatunnel_main_loop, UV_RUN_DEFAULT);

    /* cleanup any remaining open handles */
    uv_walk(codatunnel_main_loop, (uv_walk_cb)uv_close, NULL);
    uv_run(codatunnel_main_loop, UV_RUN_DEFAULT);
    uv_loop_close(codatunnel_main_loop);
    exit(0);
}


/* from Internet example */
void hexdump (char *desc, void *addr, int len)
{
    int i;
    unsigned char buff[17];
    unsigned char *pc = (unsigned char*)addr;

    // Output description if given.
    if (desc != NULL)
        printf ("%s:\n", desc);

    if (len == 0) {
        printf("  ZERO LENGTH\n");
        return;
    }
    if (len < 0) {
        printf("  NEGATIVE LENGTH: %i\n",len);
        return;
    }

    // Process every byte in the data.
    for (i = 0; i < len; i++) {
        // Multiple of 16 means new line (with line offset).

        if ((i % 16) == 0) {
            // Just don't print ASCII for the zeroth line.
            if (i != 0)
                printf ("  %s\n", buff);

            // Output the offset.
            printf ("  %04x ", i);
        }

        // Now the hex code for the specific character.
        printf (" %02x", pc[i]);

        // And store a printable ASCII character for later.
        if ((pc[i] < 0x20) || (pc[i] > 0x7e))
            buff[i % 16] = '.';
        else
            buff[i % 16] = pc[i];
        buff[(i % 16) + 1] = '\0';
    }

    // Pad out last line if not exactly 16 characters.
    while ((i % 16) != 0) {
        printf ("   ");
        i++;
    }

    // And print the final ASCII bit.
    printf ("  %s\n", buff);
}
