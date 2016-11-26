#include "tcp_in.h"
#include "counters.h"
#include "tcp_tcb.h"
#include "socket_interface.h"
#include "types.h"
#include "tcp_common.h"
#include <pthread.h>
#include <rte_mempool.h>
#include "main.h"
#include "tcp_out.h"
#include <unistd.h>
#include "tcp.h"
#include "logger.h"

enum Msg_Type {
   SOCKET_CLOSE,
   SEND_DATA,
   CONNECTION_OPEN
};

typedef struct Socket_Send_Msg_ {
   int m_Msg_Type;
   int m_Identifier; // tcb identifier
   int m_Len; // data len;
   unsigned char m_Data[1400]; // data pointer
}Socket_Send_Msg;

//static struct rte_ring *socket_tcb_ring_recv = NULL;
//static const char *TCB_TO_SOCKET = "TCB_TO_SOCKET";
//static const char *SOCKET_TO_TCB = "SOCKET_TO_TCB";
static const char *_MSG_POOL = "MSG_POOL";
static struct rte_mempool *buffer_message_pool;


void InitSocketInterface(void)
{
//   socket_tcb_ring_recv = rte_ring_lookup(TCB_TO_SOCKET);
   buffer_message_pool = rte_mempool_lookup(_MSG_POOL);
   if(buffer_message_pool == NULL) {
      printf("ERROR **** socket tcb Message pool failed\n");
   }
   else {
      printf("socket tcb recv side OK.\n");
   }
}


int 
socket_open(STREAM_TYPE stream)
{
   struct tcb *ptcb;
   (void) stream;
// future set this as default instead of 2000.
   ptcb = alloc_tcb(2000, 2000);
   printf("socket open allocated tcb %p\n", ptcb);
   return ptcb->identifier; 
}

int
sock_bridge_bind(struct sock_bridge_addr *addr)
{
   (void) addr;
   return 0;
}
 
int
socket_bind(int identifier, struct sock_addr *serv_addr)
{
   struct tcb *ptcb;

   ptcb = (struct tcb *) get_tcb_by_identifier(identifier);
   printf("socket bind received tcb %p\n", ptcb);
   if(ptcb == NULL) {
      return -1; // use enum here
   }
// add lock here. this will avoid race from other thread for ptcb.
   ptcb->ipv4_dst = serv_addr->ip;
   ptcb->dport = serv_addr->port;
   ptcb->sport = 0;
   ptcb->state = LISTENING;
   return 0;
}

int
socket_listen(int identifier, int queue_len)
{
   struct tcb *ptcb;

   (void) queue_len;
   ptcb = get_tcb_by_identifier(identifier);
   (void) ptcb;
   // not implemented yet.
   return 0; //SUCCESS
}

int
socket_accept(int ser_id, struct sock_addr *client_addr)
{
   struct tcb *ptcb = NULL;
   struct tcb *new_ptcb = NULL;
   
   (void) client_addr;
   ptcb = get_tcb_by_identifier(ser_id);
   if(ptcb->WaitingOnAccept) {
      return 0;
      // don't allow multiple accepts hold on same socket.
   }
   pthread_mutex_lock(&(ptcb->mutex));
   ptcb->state = LISTENING;
   ptcb->WaitingOnAccept = 1;
   pthread_cond_wait(&(ptcb->condAccept), &(ptcb->mutex));
   new_ptcb = ptcb->newpTcbOnAccept;
   ptcb->WaitingOnAccept = 0;
   pthread_mutex_unlock(&(ptcb->mutex));
   
   return new_ptcb->identifier; 
}


int
socket_send(int ser_id, const unsigned char *message, int len)
{
   static int counter_id = -1;
   if(counter_id == -1) {
      counter_id = create_counter("socket_sent1");
   }
   counter_inc(counter_id, len);
   Socket_Send_Msg *Msg = NULL;
   struct tcb *ptcb = get_tcb_by_identifier(ser_id);
   if (rte_mempool_get(buffer_message_pool,(void **) &Msg) < 0) {
       printf ("Failed to get message buffer\n");
/// / put assert ;
   }
   Msg->m_Identifier = ser_id;
   Msg->m_Len = len;
   Msg->m_Msg_Type = SEND_DATA;
   memcpy(Msg->m_Data, message, len);
   if (rte_ring_enqueue(ptcb->tcb_socket_ring_send, Msg) < 0) {
      printf("Failed to send message - message discarded\n");
      rte_mempool_put(buffer_message_pool, Msg);
   }
   else {
        static int counter_id = -1;
        if(counter_id == -1) {
           counter_id = create_counter("socket_sent");
        }
        counter_inc(counter_id, len);
        printf("****** Enqued len %d and identifier %d\n", Msg->m_Len, Msg->m_Identifier);
   }
  // sendtcppacket(ptcb, mbuf, message, len);
  // ptcb->send_data(message, len); 
   return 0;
}

extern struct tcb *tcbs[];
extern int Ntcb;
extern pthread_mutex_t tcb_alloc_mutex;
int
check_socket_out_queue(void)
{
   Socket_Send_Msg *msg;
   uint32_t i;
   unsigned char message[1500];
   struct tcb *ptcb = NULL;
   struct tcb *temp = NULL;
   pthread_mutex_lock(&tcb_alloc_mutex);
   uint32_t TotalTcbs = Ntcb;
   pthread_mutex_unlock(&tcb_alloc_mutex);
    
   for(i=0; i<TotalTcbs; i++) {  // change it to hash type later
   //   if( i != 0)
      //printf("Checking tcb %d for sendingi %d %d\n", i, Ntcb, TotalTcbs);
      ptcb = tcbs[i];
      if(ptcb == NULL) {
         continue;
      }
      int num = rte_ring_dequeue(ptcb->tcb_socket_ring_recv, (void **)&msg);
      if(num < 0) {
         if(ptcb->need_ack_now) {
            logger(LOG_TCP, LOG_LEVEL_NORMAL, "sending immidiate ack for tcb %u\n", ptcb->identifier);
            ptcb->tcp_flags = TCP_FLAG_ACK;
            sendtcpdata(ptcb, NULL, 0);
            ptcb->need_ack_now = 0;
            // will use a saeparate api for ack later.
         }
         continue;
      }
      if(msg->m_Msg_Type == SOCKET_CLOSE) {
         temp = get_tcb_by_identifier(msg->m_Identifier);
         if(temp != ptcb) {
            printf ("Everything screewed at tcb'\n");
            exit(0);
      // put assert
         }
         ptcb->tcp_flags = TCP_FLAG_ACK | TCP_FLAG_FIN; // no problem in acking
         ptcb->need_ack_now = 1;
    //     sendfin(ptcb);
      }
      if(msg->m_Msg_Type == CONNECTION_OPEN) {
         temp = get_tcb_by_identifier(msg->m_Identifier);
         if(temp != ptcb) {
            printf ("Everything screewed at tcb'\n");
            exit(0);
      // put assert
         }
         sendsyn(ptcb);
         ptcb->state = SYN_SENT; 
      }
      if(msg->m_Msg_Type == SEND_DATA) {
         printf("****** Received len %d and identifier %d\n", msg->m_Len, msg->m_Identifier);
         memcpy(message, msg->m_Data, msg->m_Len);
     
         temp = get_tcb_by_identifier(msg->m_Identifier);
         if(temp != ptcb) {
            printf ("Everything screewed at tcb'\n");
            exit(0);
      // put assert
         }
         ptcb->tcp_flags = TCP_FLAG_ACK; 
         printf("sending data to tcp\n");
         sendtcpdata(ptcb, message, msg->m_Len);
      }
      rte_mempool_put(buffer_message_pool, msg);
   }
   return 0;
}

int
socket_read(int ser_id, char *buffer, int len)
{
// check all corner case before going further
//   assert(ptcb->WaitingOnRead == 0);
   struct tcb *ptcb = NULL;
   ptcb = get_tcb_by_identifier(ser_id);
 //  if(ptcb->WaitingOnAccept) {
   //   return 0;
      // don't allow multiple accepts hold on same socket.
  // }
   printf("scoket read called for identifier %d\n", ser_id);
   pthread_mutex_lock(&(ptcb->mutex));
   ptcb->WaitingOnRead = 1;
   pthread_cond_wait(&(ptcb->condAccept), &(ptcb->mutex));
   if(len < ptcb->read_buffer_len) {
      printf("ERROR: Failed to get all buffer data from read.\n");
      memcpy(buffer, ptcb->read_buffer, len); 
   }   
   else 
      memcpy(buffer, ptcb->read_buffer, ptcb->read_buffer_len); 
   ptcb->WaitingOnRead = 0;
   pthread_mutex_unlock(&(ptcb->mutex));
   
   return 10; 
}

int socket_connect(int identifier, struct sock_addr *client_addr)
{
/* using static ip for current. furute get ip from conf*/
   int i;
      uint8_t ip[4];
      ip[0] = 192;
      ip[1] = 168;
      ip[2] = 78;
      ip[3] = 2;
   uint32_t DestIp = 0;
   static uint16_t SrcPorts = 0;
   if(SrcPorts == 0) {
      SrcPorts = 10000;
   }
   SrcPorts ++;
   for(i=0; i<4; i++) {
      DestIp |= ip[i] << i*8;
   }
   printf("opening connection connect call\n");
   Socket_Send_Msg *Msg = NULL;
   struct tcb *ptcb = get_tcb_by_identifier(identifier);
   if (rte_mempool_get(buffer_message_pool,(void **) &Msg) < 0) {
       printf ("Failed to get message buffer\n");
/// / put assert ;
   }
   Msg->m_Identifier = identifier;
   Msg->m_Msg_Type = CONNECTION_OPEN;
   if (rte_ring_enqueue(ptcb->tcb_socket_ring_send, Msg) < 0) {
      printf("Failed to send message - message discarded\n");
      rte_mempool_put(buffer_message_pool, Msg);
   }
   ptcb->ipv4_src = htonl(client_addr->ip); 
   ptcb->sport = client_addr->port; 
   ptcb->ipv4_dst = DestIp; 
   ptcb->dport = SrcPorts; 
   ptcb->next_seq = 1;
   pthread_mutex_lock(&(ptcb->mutex));
   ptcb->WaitingOnConnect = 1;
   pthread_cond_wait(&(ptcb->condAccept), &(ptcb->mutex));
   ptcb->WaitingOnConnect = 0;
   pthread_mutex_unlock(&(ptcb->mutex));
// wait on sema event of syn-ack.
 //  remove_tcb(identifier);
   return 0;
}

int socket_read_nonblock(int ser_id, unsigned char *buffer)
{
   void *msg;
   struct tcb *ptcb = get_tcb_by_identifier(ser_id);
   while (rte_ring_dequeue(ptcb->socket_tcb_ring_recv, &msg) < 0){
      usleep(5);
      continue;
   }
//   printf("Received %s and len %d\n",(char *)msg, strlen(msg));
   memcpy(buffer, msg, strlen(msg));
   rte_mempool_put(buffer_message_pool, msg);
 
   return strlen(msg);//GetData(ser_id, buffer);
}

int
socket_close(int identifier)
{
   printf("closing tcb\n");
   Socket_Send_Msg *Msg = NULL;
   struct tcb *ptcb = get_tcb_by_identifier(identifier);
   if (rte_mempool_get(buffer_message_pool, (void **)&Msg) < 0) {
       printf ("Failed to get message buffer\n");
/// / put assert ;
   }
   Msg->m_Identifier = identifier;
   Msg->m_Msg_Type = SOCKET_CLOSE;
   if (rte_ring_enqueue(ptcb->tcb_socket_ring_send, Msg) < 0) {
      printf("Failed to send message - message discarded\n");
      rte_mempool_put(buffer_message_pool, Msg);
   }
 //  remove_tcb(identifier);
   return 0;
}
