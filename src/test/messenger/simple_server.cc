// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 CohortFS, LLC
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <sys/types.h>

#include <iostream>
#include <string>

using namespace std;

#include "common/config.h"
#include "msg/Messenger.h"
#include "common/Timer.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "global/signal_handler.h"
#include "perfglue/heap_profiler.h"
#include "common/address_helper.h"
#include "simple_dispatcher.h"

#define dout_subsys ceph_subsys_simple_server


int main(int argc, const char **argv)
{
	vector<const char*> args;
	Messenger *messenger;
	Dispatcher *dispatcher;
	std::vector<const char*>::iterator arg_iter;
	std::string val;
	entity_addr_t bind_addr;
	int r = 0;

	using std::endl;

	std::string addr = "localhost";
	std::string port = "1234";

	cout << "Simple Server starting..." << endl;

	argv_to_vec(argc, argv, args);
	env_to_vec(args);

	auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_ANY,
			       CODE_ENVIRONMENT_DAEMON,
			       0);

	for (arg_iter = args.begin(); arg_iter != args.end();) {
	  if (ceph_argparse_witharg(args, arg_iter, &val, "--addr",
				    (char*) NULL)) {
	    addr = val;
	  } else if (ceph_argparse_witharg(args, arg_iter, &val, "--port",
				    (char*) NULL)) {
	    port = val;
	  } else {
	    ++arg_iter;
	  }
	};

	string dest_str = "tcp://";
	dest_str += addr;
	dest_str += ":";
	dest_str += port;
	entity_addr_from_url(&bind_addr, dest_str.c_str());
	// 创建Messenger实例
	messenger = Messenger::create(g_ceph_context, g_conf->get_val<std::string>("ms_type"), // 配置的实现类型
				      entity_name_t::MON(-1),
				      "simple_server",
				      0 /* nonce */,
				      0 /* flags */);
	// 设置Messenger属性
	messenger->set_magic(MSG_MAGIC_TRACE_CTR);
	messenger->set_default_policy(
	  Messenger::Policy::stateless_server(0));
	// 绑定地址
	r = messenger->bind(bind_addr);
	if (r < 0)
		goto out;

	// Set up crypto, daemonize, etc.
	//global_init_daemonize(g_ceph_context, 0);
	common_init_finish(g_ceph_context);
	// 创建分发器
	dispatcher = new SimpleDispatcher(messenger);
	// 添加到Messenger
	messenger->add_dispatcher_head(dispatcher); // should reach ready()
	messenger->start(); // 启动
	messenger->wait(); // can't be called until ready()

	// done
	delete messenger;

out:
	cout << "Simple Server exit" << endl;
	return r;
}

