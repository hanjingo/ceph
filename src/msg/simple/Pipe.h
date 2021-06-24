// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_MSGR_PIPE_H
#define CEPH_MSGR_PIPE_H

#include "include/memory.h"
#include "auth/AuthSessionHandler.h"

#include "msg/msg_types.h"
#include "msg/Messenger.h"
#include "PipeConnection.h"


class SimpleMessenger;
class DispatchQueue;

static const int SM_IOV_MAX = (IOV_MAX >= 1024 ? IOV_MAX / 4 : IOV_MAX);

  /**
   * The Pipe is the most complex SimpleMessenger component. It gets
   * two threads, one each for reading and writing on a socket it's handed
   * at creation time, and is responsible for everything that happens on
   * that socket. Besides message transmission, it's responsible for
   * propagating socket errors to the SimpleMessenger and then sticking
   * around in a state where it can provide enough data for the SimpleMessenger
   * to provide reliable Message delivery when it manages to reconnect.
   */
  class Pipe : public RefCountedObject {
    /**
     * 接收线程
     *    用来接收数据
     *
     * 
     */
    class Reader : public Thread {
      Pipe *pipe;
    public:
      explicit Reader(Pipe *p) : pipe(p) {}
      void *entry() override { pipe->reader(); return 0; }
    } reader_thread;

    /**
     * 发送线程
     *    用来发送数据
     */
    class Writer : public Thread {
      Pipe *pipe;
    public:
      explicit Writer(Pipe *p) : pipe(p) {}
      void *entry() override { pipe->writer(); return 0; }
    } writer_thread;

    class DelayedDelivery;
    DelayedDelivery *delay_thread;
  public:
    Pipe(SimpleMessenger *r, int st, PipeConnection *con);
    ~Pipe() override;
    // msgr的指针
    SimpleMessenger *msgr;
    uint64_t conn_id; // 分配给Pipe自己唯一的id
    ostream& _pipe_prefix(std::ostream &out) const;

    Pipe* get() {
      return static_cast<Pipe*>(RefCountedObject::get());
    }

    bool is_connected() {
      Mutex::Locker l(pipe_lock);
      return state == STATE_OPEN;
    }

    char *recv_buf;           // 接收缓冲区
    size_t recv_max_prefetch; // 接收缓冲区一次预约的最大值
    size_t recv_ofs;          // 接收的偏移量
    size_t recv_len;          // 接收的长度

    enum {
      STATE_ACCEPTING,
      STATE_CONNECTING,
      STATE_OPEN,
      STATE_STANDBY,
      STATE_CLOSED,
      STATE_CLOSING,
      STATE_WAIT       // just wait for racing connection
    };

    static const char *get_state_name(int s) {
      switch (s) {
      case STATE_ACCEPTING: return "accepting";
      case STATE_CONNECTING: return "connecting";
      case STATE_OPEN: return "open";
      case STATE_STANDBY: return "standby";
      case STATE_CLOSED: return "closed";
      case STATE_CLOSING: return "closing";
      case STATE_WAIT: return "wait";
      default: return "UNKNOWN";
      }
    }
    const char *get_state_name() {
      return get_state_name(state);
    }

  private:
    int sd;                           // pipe对应的sockedfd
    struct iovec msgvec[SM_IOV_MAX];  // 发送消息的iovec结构

  public:
    int port;                 // 链接端口
    int peer_type;            // 链接对方的类型
    entity_addr_t peer_addr;  // 对方地址
    Messenger::Policy policy; // 策略
    
    Mutex pipe_lock;
    int state;                                  // 当前链接的状态
    std::atomic<bool> state_closed = { false }; // 状态是否为STATE_CLOSED

    // session_security handles any signatures or encryptions required for this pipe's msgs. PLR

    ceph::shared_ptr<AuthSessionHandler> session_security;

  protected:
    friend class SimpleMessenger;
    PipeConnectionRef connection_state; // PipeConnection的引用

    utime_t backoff;         // backoff time

    bool reader_running, reader_needs_join;
    bool reader_dispatching; /// reader thread is dispatching without pipe_lock
    bool notify_on_dispatch_done; /// something wants a signal when dispatch done
    bool writer_running;

    map<int, list<Message*> > out_q;    // 准备发送的消息优先队列
    DispatchQueue *in_q;                // 接收消息的队列
    list<Message*> sent;                // 要发送的消息
    Cond cond;
    bool send_keepalive;
    bool send_keepalive_ack;
    utime_t keepalive_ack_stamp;
    bool halt_delivery;                 // 如果Pipe队列销毁，停止增加
    
    __u32 connect_seq, peer_global_seq;
    uint64_t out_seq;                   // 发送消息的序列号
    uint64_t in_seq, in_seq_acked;      // 接收到消息序号和ACK的序号
    
    void set_socket_options();

    int accept();   // server handshake
    int connect();  // client handshake
    void reader();
    void writer();
    void unlock_maybe_reap();

    int randomize_out_seq();

    int read_message(Message **pm,
		     AuthSessionHandler *session_security_copy);
    int write_message(const ceph_msg_header& h, const ceph_msg_footer& f, bufferlist& body); // 发送header,footer,数据list
    /**
     * Write the given data (of length len) to the Pipe's socket. This function
     * will loop until all passed data has been written out.
     * If more is set, the function will optimize socket writes
     * for additional data (by passing the MSG_MORE flag, aka TCP_CORK).
     *
     * @param msg The msghdr to write out
     * @param len The length of the data in msg
     * @param more Should be set true if this is one part of a larger message
     * @return 0, or -1 on failure (unrecoverable -- close the socket).
     */
    int do_sendmsg(struct msghdr *msg, unsigned len, bool more=false);
    int write_ack(uint64_t s);
    int write_keepalive();
    int write_keepalive2(char tag, const utime_t &t);

    void fault(bool reader=false);

    void was_session_reset();

    /* Clean up sent list */
    void handle_ack(uint64_t seq);

    public:
    Pipe(const Pipe& other);
    const Pipe& operator=(const Pipe& other);

    void start_reader();
    void start_writer();
    void maybe_start_delay_thread();
    void join_reader();

    // public constructors
    static const Pipe& Server(int s);
    static const Pipe& Client(const entity_addr_t& pi);

    uint64_t get_out_seq() { return out_seq; }

    bool is_queued() { return !out_q.empty() || send_keepalive || send_keepalive_ack; }

    entity_addr_t& get_peer_addr() { return peer_addr; }

    void set_peer_addr(const entity_addr_t& a) {
      if (&peer_addr != &a)  // shut up valgrind
        peer_addr = a;
      connection_state->set_peer_addr(a);
    }
    void set_peer_type(int t) {
      peer_type = t;
      connection_state->set_peer_type(t);
    }

    void register_pipe();
    void unregister_pipe();
    void join();
    /// stop a Pipe by closing its socket and setting it to STATE_CLOSED
    void stop();
    /// stop() a Pipe if not already done, and wait for it to finish any
    /// fast_dispatch in progress.
    void stop_and_wait();

    void _send(Message *m) {
      assert(pipe_lock.is_locked());
      out_q[m->get_priority()].push_back(m);
      cond.Signal();
    }
    void _send_keepalive() {
      assert(pipe_lock.is_locked());
      send_keepalive = true;
      cond.Signal();
    }
    Message *_get_next_outgoing() {
      assert(pipe_lock.is_locked());
      Message *m = 0;
      while (!m && !out_q.empty()) { // 从out_q移除消息
        map<int, list<Message*> >::reverse_iterator p = out_q.rbegin();
        if (!p->second.empty()) {
          m = p->second.front();
          p->second.pop_front();
        }
        if (p->second.empty())
          out_q.erase(p->first);
      }
      return m;
    }

    /// move all messages in the sent list back into the queue at the highest priority.
    void requeue_sent();
    /// discard messages requeued by requeued_sent() up to a given seq
    void discard_requeued_up_to(uint64_t seq);
    void discard_out_queue();

    void shutdown_socket() {
      recv_reset();
      if (sd >= 0)
        ::shutdown(sd, SHUT_RDWR);
    }

    void recv_reset() {
      recv_len = 0;
      recv_ofs = 0;
    }
    ssize_t do_recv(char *buf, size_t len, int flags);
    ssize_t buffered_recv(char *buf, size_t len, int flags);
    bool has_pending_data() { return recv_len > recv_ofs; }

    /**
     * do a blocking read of len bytes from socket
     *
     * @param buf buffer to read into
     * @param len exact number of bytes to read
     * @return 0 for success, or -1 on error
     */
    int tcp_read(char *buf, unsigned len);

    /**
     * wait for bytes to become available on the socket
     *
     * @return 0 for success, or -1 on error
     */
    int tcp_read_wait();

    /**
     * non-blocking read of available bytes on socket
     *
     * This is expected to be used after tcp_read_wait(), and will return
     * an error if there is no data on the socket to consume.
     *
     * @param buf buffer to read into
     * @param len maximum number of bytes to read
     * @return bytes read, or -1 on error or when there is no data
     */
    ssize_t tcp_read_nonblocking(char *buf, unsigned len);

    /**
     * blocking write of bytes to socket
     *
     * @param buf buffer
     * @param len number of bytes to write
     * @return 0 for success, or -1 on error
     */
    int tcp_write(const char *buf, unsigned len);

  };


#endif
