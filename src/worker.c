// $Id$    --*- c++ -*--

// Copyright (C) 2003 Enrico Scholz <enrico.scholz@informatik.tu-chemnitz.de>
//  
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; version 2 of the License.
//  
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//  
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
//  

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "worker.h"
#include "jobinfo.h"
#include "wrappers.h"
#include "prioqueue.h"
#include "parameters.h"
#include "util.h"

#include <assert.h>
#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/types.h>

static struct ether_addr const	BCAST_MAC   = { { 255, 255, 255, 255, 255, 255 } };

static void	Worker_run(struct Worker *worker);

void
Worker_init(struct Worker *worker, struct Arguments const *args,
	    int sock, int if_idx)
{
  pid_t		pid;
  int		fds[2];
  
  assert(worker!=0);

  worker->sock   = sock;
  worker->if_idx = if_idx;
  worker->llmac  = args->llmac;

  Epipe(fds);
  pid = Efork();

  if (pid==0) {
    Eclose(fds[1]);
    worker->fd = fds[0];
    Worker_run(worker);
    close(worker->fd);
    exit(1);	// worker never exits
  }
  else {
    Eclose(fds[0]);
    worker->fd = fds[1];
  }
}

#ifdef ENSC_TESTSUITE
void
Worker_free(struct Worker *worker)
{
  close(worker->fd);
}
#endif

void
Worker_sendJob(struct Worker *worker, struct RequestInfo const *request)
{
  static int		error_count = 0;
  int			ret;

    // cleanup died child
  wait4(0,0,WNOHANG,0);

  ret = TEMP_FAILURE_RETRY(write(worker->fd, request, sizeof(*request)));
  if (ret!=sizeof(*request)) {
    ++error_count;
    if (error_count>MAX_ERRORS) {
      FatalErrnoError(ret==-1, 1, "write()");
      WRITE_MSG(2, "write() could not sent all bytes to worker-process; aborting...\n");
      exit(1);
    }
    
    sleep(1);
    return;
  }

  error_count = 0;
}

static void
Worker_executeJob(struct Worker const *worker, struct ScheduleInfo const *job)
{
  static int		error_count = 0;
    // prevent ugly casts...
  void const *		addr_v = &job->address;
  int			ret;

  ret = sendto(worker->sock, &job->message, sizeof(job->message),
	       0, addr_v, sizeof(job->address));
  if (ret!=sizeof(job->message)) {
    ++error_count;
    if (error_count>MAX_ERRORS) {
      FatalErrnoError(ret==-1, 1, "sendto()");
      WRITE_MSG(2, "sendto() could not sent all bytes; aborting...\n");
      exit(1);
    }
    sleep(1);
    return;
  }

  error_count = 0;
}

static void ALWAYSINLINE
Worker_fillPacket(struct Worker const *worker,
		  struct ScheduleInfo *job,
		  struct RequestInfo const *rq)
{
  ArpMessage * const		msg  = &job->message;
  struct sockaddr_ll * const	addr = &job->address;
  void const *			dhost_ptr;
  struct ether_addr		shost_ether;
  struct ether_addr const *	shost_ptr;
  register in_addr_t * const		arp_tpa_ptr = reinterpret_cast(in_addr_t*)(&msg->data.arp_tpa);
  register in_addr_t const * const	rq_tpa_ptr  = reinterpret_cast(in_addr_t*)(&rq->request.arp_tpa);

  assert(worker!=0);
  assert(job!=0);
  assert(rq!=0);
  
  memset(msg,  0, sizeof(*msg));
  memset(addr, 0, sizeof(*addr));

  switch (rq->type) {
    case jobDST		:
      shost_ptr    = &rq->mac;
      dhost_ptr    = BCAST_MAC.ether_addr_octet;
      *arp_tpa_ptr = INADDR_ANY;
      break;
    case jobSRC		:
      shost_ptr    = 0;
      dhost_ptr    = rq->request.arp_sha;
      *arp_tpa_ptr = *rq_tpa_ptr;
      break;
    default		:
      assert(false);
      WRITE_MSGSTR(2, "unknown 'rq->type' in Worker_fillPacket(); aborting...\n");
      exit(1);
      break;
  }

  if (shost_ptr==0) {
    switch (worker->llmac.type) {
      case mcSAME	:  shost_ptr = &rq->mac;                  break;
      case mcFIXED	:  shost_ptr = &worker->llmac.addr.ether; break;
      case mcRANDOM	:
	Util_setRandomMac(&shost_ether);
	shost_ptr = &shost_ether;
	break;
      default		:
	assert(false);
	WRITE_MSGSTR(2, "unknown 'worker->llmac.type' in Worker_fillPacket(); aborting...\n");
	exit(1);
	break;
    }
  }

  assert(shost_ptr!=0);
  assert(dhost_ptr!=0);
  
  msg->padding[0] = 0x66;
  msg->padding[1] = 0x60;
  msg->padding[sizeof(msg->padding)-2] = 0x0B;
  msg->padding[sizeof(msg->padding)-1] = 0x5E;

  msg->data.ea_hdr.ar_hrd = htons(ARPHRD_ETHER);
  msg->data.ea_hdr.ar_pro = htons(ETHERTYPE_IP);
  msg->data.ea_hdr.ar_hln = ETHER_ADDR_LEN;
  msg->data.ea_hdr.ar_pln = 4;
  msg->data.ea_hdr.ar_op  = htons(ARPOP_REPLY);

  memcpy(msg->data.arp_sha, &rq->mac,   sizeof(msg->data.arp_sha));
  memcpy(msg->data.arp_spa, rq_tpa_ptr, sizeof(msg->data.arp_spa));
  memcpy(msg->data.arp_tha, dhost_ptr,  sizeof(msg->data.arp_tha));

  msg->header.ether_type  = htons(ETH_P_ARP);

  memcpy(msg->header.ether_shost, shost_ptr,      sizeof(msg->header.ether_shost));
  memcpy(msg->header.ether_dhost, dhost_ptr,      sizeof(msg->header.ether_dhost));


  addr->sll_family   = AF_PACKET;
  addr->sll_ifindex  = worker->if_idx;
  addr->sll_halen    = ETHER_ADDR_LEN;

  memcpy(addr->sll_addr, dhost_ptr, ETHER_ADDR_LEN);
}

inline static char const *
arpinet_ntoa(void const *ptr_v)
{
  struct in_addr const * const	ptr = ptr_v;
  assert(ptr!=0);

  return inet_ntoa(*ptr);
}

inline static char const *
arpether_ntoa(void const *ptr_v)
{
  struct ether_addr const * const	ptr = ptr_v;
  assert(ptr!=0);

  return ether_ntoa(ptr);
}

static void ALWAYSINLINE
Worker_printJob(struct RequestInfo const *rq)
{
  writeMsgTimestamp(1);
#if 0
#warning !!! Legacy log-format enabled; support may be dropped without explicit warnings !!!
  WRITE_MSGSTR(1, ": Handle IP '");
  WRITE_MSG   (1, arpinet_ntoa (rq->request.arp_tpa));
  WRITE_MSGSTR(1, "' requested by '");
  WRITE_MSG   (1, arpinet_ntoa (rq->request.arp_spa));
  WRITE_MSGSTR(1, "' [");
  WRITE_MSG   (1, arpether_ntoa(rq->request.arp_sha));
  WRITE_MSGSTR(1, "]\n");
#else
  WRITE_MSGSTR(1, ": ");
  WRITE_MSG   (1, arpinet_ntoa (rq->request.arp_spa));
  WRITE_MSGSTR(1, "/");
  WRITE_MSG   (1, arpether_ntoa(rq->request.arp_sha));
  if      (rq->type==jobSRC) WRITE_MSGSTR(1, " !-> ");
  else if (rq->type==jobDST) WRITE_MSGSTR(1, " ->! ");
  else                       WRITE_MSGSTR(1, " ERR ");
  WRITE_MSG   (1, arpinet_ntoa (rq->request.arp_tpa));
  WRITE_MSGSTR(1, "/");
  WRITE_MSG   (1, arpether_ntoa(rq->request.arp_tha));
  WRITE_MSGSTR(1, " [");
  WRITE_MSG   (1, arpether_ntoa(&rq->mac));
  WRITE_MSGSTR(1, "]\n");
#endif
}

static void ALWAYSINLINE
Worker_scheduleNewJob(struct Worker *worker, struct PriorityQueue *queue, time_t now)
{
  struct RequestInfo	request;
  struct ScheduleInfo	job;
  static int		error_count = 0;
  size_t		cnt = read(worker->fd, &request, sizeof(request));

  if (cnt!=sizeof(request)) {
    ++error_count;
    if (error_count>MAX_ERRORS) {
      FatalErrnoError(cnt==reinterpret_cast(size_t)(-1), 1, "read()");
      WRITE_MSG(2, "read() returns invalid RequestInfo; aborting...\n");
      exit(1);
    }
    sleep(1);
    return;
  }

  Worker_fillPacket(worker, &job, &request);
  Worker_executeJob(worker, &job);

  Worker_printJob(&request);
  
  if (PriorityQueue_count(queue)>=MAX_REQUESTS) {
    writeMsgTimestamp(2);
    WRITE_MSGSTR(2, ": Too much requests scheduled (");
    writeUInt(2, PriorityQueue_count(queue));
    WRITE_MSGSTR(2, ") sleeping a while...\n");
    sleep(1);
  }
  
  job.schedule_time = now+2;
  PriorityQueue_insert(queue, &job);

  job.schedule_time = now+5;
  PriorityQueue_insert(queue, &job);

  error_count = 0;
}

int
Worker_jobCompare(void const *lhs_v, void const *rhs_v)
{
  struct ScheduleInfo const *	lhs = lhs_v;
  struct ScheduleInfo const *	rhs = rhs_v;

  assert(lhs!=0);
  assert(rhs!=0);

  return -(lhs->schedule_time - rhs->schedule_time);
}

static void
Worker_run(struct Worker *worker)
{
  struct PriorityQueue	jobqueue;
  int			error_count = 0;

  PriorityQueue_init(&jobqueue, Worker_jobCompare, 1023, sizeof(struct ScheduleInfo));
  
  for (;;) {
    struct timeval		timeout;
    struct timeval *		time_ptr;
    struct ScheduleInfo const *	job = PriorityQueue_max(&jobqueue);
    time_t			now = time(0);
    fd_set			read_set;

    if (job==0) time_ptr = 0;
    else {
      timeout.tv_sec  = (job->schedule_time>now) ? (job->schedule_time-now) : 0;
      timeout.tv_usec = 0;
      time_ptr        = &timeout;
    }

    FD_ZERO(&read_set);
    FD_SET(worker->fd, &read_set);
    
    if (select(worker->fd+1, &read_set, 0,0, time_ptr)==-1) {
      ++error_count;
      if (error_count>MAX_ERRORS) {
	FatalErrnoError(1, 1, "select()");
	exit(1);
      }
      sleep(1);
      continue;
    }

    if (FD_ISSET(worker->fd, &read_set))
      Worker_scheduleNewJob(worker, &jobqueue, time(0));

    now = time(0);
    for (;;) {
      job = PriorityQueue_max(&jobqueue);
      if (job==0 || now<job->schedule_time) break;
      
      Worker_executeJob(worker, job);
      PriorityQueue_extract(&jobqueue);
    }

    error_count = 0;
  }
}

#ifdef ENSC_TESTSUITE
void
Worker_printScheduleInfo(int fd, struct ScheduleInfo const *job)
{
  struct ether_header const * const	eth = &job->message.header;
  struct ether_arp const * const	arp = &job->message.data;
  struct sockaddr_ll const* const	sll = &job->address;

  assert(eth->ether_type==htons(ETH_P_ARP));

  WRITE_MSGSTR(fd, "ether=[");
  WRITE_MSG   (fd, arpether_ntoa(eth->ether_dhost));
  WRITE_MSGSTR(fd, ",");
  WRITE_MSG   (fd, arpether_ntoa(eth->ether_shost));
  WRITE_MSGSTR(fd, "], arp=[");
  WRITE_MSG   (fd, arpether_ntoa(arp->arp_sha));
  WRITE_MSGSTR(fd, ",");
  WRITE_MSG   (fd, arpinet_ntoa (arp->arp_spa));
  WRITE_MSGSTR(fd, ", ");
  WRITE_MSG   (fd, arpether_ntoa(arp->arp_tha));
  WRITE_MSGSTR(fd, ",");
  WRITE_MSG   (fd, arpinet_ntoa (arp->arp_tpa));
  WRITE_MSGSTR(fd, "], sock=[");
  WRITE_MSG   (fd, arpether_ntoa(sll->sll_addr));
  WRITE_MSGSTR(fd, "]\n");
}

void
Worker_debugFillPacket(struct Worker const *worker,
		       struct ScheduleInfo *job,
		       struct RequestInfo const *rq)
{
  Worker_fillPacket(worker, job, rq);
}
#endif
