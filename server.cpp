﻿#include <sys/epoll.h>
#include <sys/types.h>  
#include <sys/socket.h> 
#include <arpa/inet.h>  
#include <sys/resource.h>
#include <signal.h> 
#include "server.h"

server::server()
{
	m_epfd = -1;
	m_listenfd  = -1;
	m_listen_port  = -1;
	m_app_num = 0;
	m_last_app = -1;
	memset(m_apps, 0, sizeof(m_apps));
	m_keepalive_timeout = 120;
}


server:: ~server()
{
	stop();
}

server * server::instance(){
	static server inst;
	return &inst;
}

int server::run()
{
	int event_num = 1024 * 8;
	struct epoll_event * events = (struct epoll_event*)malloc(sizeof(struct epoll_event) * event_num); 
	
	if(!events){
		return -1;
	}
	
	while(true){
	
		int timeout = m_timer.timeout();
		
		int res = epoll_wait (m_epfd, events, event_num, timeout);
		
		if (res == -1)
		{
			printf_t("error: epoll_wait error(%d %s)\n", errno, strerror(errno));
			if (errno != EINTR) 
			{
				return -1;
			}
			continue;
		}
		
		if(timeout == 0){
			printf_t("debug: epoll_wait timeout %d\n", res);
		}
		
		time_t cur = time(NULL);
				
		for(int i = 0; i < res; i++)
		{
			//accept
			if (events[i].data.ptr == &m_listenfd)
			{	
				handle_accept();
				continue;
			}
			
			connection * n = (connection*)events[i].data.ptr;
			
			//read
			if (events[i].events & EPOLLIN)
			{
				n->set_alive_time(cur);
				if(handle_recv(n) < 0)
				{
					handle_close(n, recv_reason);
					continue;
				}
			}
			
			//write
			if(events[i].events & EPOLLOUT)
			{
				if(handle_write(n) <0)
				{
					handle_close(n, write_reason);
					continue;
				}
			}
			
			//error
			if (events[i].events & (EPOLLERR | EPOLLHUP ))
			{
				handle_close(n, error_reason);
			}
		}
	
		//timer event
		evtime ev ;
		while(m_timer.pop_timeout(ev) != -1)
		{	
			handle_timer(&ev);
		}
	}
	
	if(events)
	{
		free(events);
	}
	
	return 0;
}

int server::get_tcp_port()
{
	return 	m_listen_port;
}
	

/*
void* ptr;     //消息节点，可以为空
	int appid;
	int from;  
	int event;      //消息
	int length;     //消息长度 
	char* content;  //消息内容
*/
int server::post_msg(int appid, hd_app * temp)
{
	if(appid < 0 || appid >= m_app_num)
	{
		printf_t("error: appid error\n");
		return -1;
	}
	
	app * a = m_apps[appid];
	if(!a)
	{
		printf_t("error: no app bind\n");
		return -1;
	}
	
	//md by 2015-1-20
	hd_app * msg = (hd_app*)malloc(sizeof(hd_app) + temp->length + 1);
	if(!msg)
	{
		printf_t("error: malloc %d bytes fail, error(%d)\n", sizeof(hd_app) + temp->length + 1, errno);
		return -1;
	}
	memcpy(msg, temp, sizeof(hd_app));
	
	if(temp->length){
		msg->content = (char*)msg + sizeof(hd_app) ;
		memcpy(msg->content, temp->content, temp->length);
		msg->content[temp->length] = 0;	//add by 2015-1-20
	}
	
	if(a->push(msg) < 0)
	{
		int total = a->increase_drop_msg();
		free(msg);	
		printf_t("error: drop msg count(%d)\n", total);
		return -1;
	}
	return 0;
}

int server::post_app_msg(int dst, int event, void * content, int length)
{
	hd_app  msg = {0};
	msg.event = event;
	msg.content = (char*)content;
	msg.length = length;
	msg.type = app_type;

	return post_msg(dst, &msg);
}

int server::post_tcp_msg(connection * n, int event, void * content, int length)
{
	hd_app  msg = {0};
	msg.event = event;
	msg.content = (char*)content;
	msg.length = length;
	msg.type = tcp_type;
	msg.u.tcp.n = n;
	return post_msg(n->get_appid(), &msg);
}

int server::post_timer_msg(evtime * e)
{
	hd_app msg = {0};
	msg.event = e->id;
	msg.type = timer_type;
	msg.u.timer.ptr = e->ptr;
	msg.u.timer.interval = e->interval;
	return post_msg(e->appid, &msg);
}

int server::get_appid()
{
	return m_last_app = (m_last_app + 1) % m_app_num;
}

int server::packet_dispatch(connection * n)
{
	packet_buf * p_buf = n->get_recv_buf();

	int offset = 0;
	int dst = n->get_appid();
	if(dst < 0 || dst >= m_app_num){
		printf_t("packet_dispatch error appid %d\n", dst);
		return -1;
	}
	
	app * a = m_apps[dst];
	if(!a){
		printf_t("packet_dispatch invalid appid %d\n", dst);
		return -1;
	}
	
	while(p_buf->has > 0)
	{
		char* packet = NULL;
		int pktlen = 0;
		int consume = a->on_unpack(p_buf->buf + offset, p_buf->has ,pktlen, packet);
		
		if (consume < 0 || consume > p_buf->has){
			printf_t("unpack error consume(%d) has(%d)\n", consume, p_buf->has);
			return -1;
		}

		if (!consume){
			if(p_buf->has && offset){
				memcpy(p_buf->buf, p_buf->buf + offset, p_buf->has);
			}
			return 0;
		}

		if (pktlen < 0  || pktlen > consume){
			printf_t("unpack error pktlen(%d) consume(%d)\n", pktlen, consume);
			return -1;
		}
		
		p_buf->has -= consume;
		offset += consume;
		
		//ignore empty packet
		if(pktlen && packet){
			post_tcp_msg(n, ev_recv, packet, pktlen);	
		}	
	}
	
	return 0;
}


int server::check_keepalive(evtime * e)
{
	time_t cur = time(NULL);
	connection * n = m_con_list.go_first();
	while(n)
	{
		if(n->get_status() != kconnected)
		{	
			n = m_con_list.go_next();
			continue;
		}
		
		int dis = (int)(cur  - n->get_alive_time());
		if(dis < m_keepalive_timeout)
		{
			break;
		}
		printf_t("warn : connection(%d) time out(%d)\n", n->fd(), dis);
		
		//fixed 2014-12-26
		connection * next =  m_con_list.go_next();
		handle_close(n, timeout_reason);
		n = next;
	}
	m_timer.add(e->id, e->interval, e->ptr);
	return 0;
}


int server::check_invalid_con(evtime * e)
{
	connection * n = m_con_list.go_first();
	while(n)
	{	
		if(n->get_status() != kdisconnected)
		{
			break;
		}
		
		if(n->get_status() == kdisconnected && n->expired())
		{
			connection * next = m_con_list.go_next();
			m_con_list.remove(n);
			delete n;
			n = next;
			//printf_t("debug: invalid node delete\n");
		}
		else{
			n = m_con_list.go_next();
		}
	}
	m_timer.add(e->id, e->interval, e->ptr);
	return 0;
}


int server::start_connect(evtime * e)
{
	connection * n = (connection*)e->ptr;
	ipaddr peeraddr = n->get_peeraddr();

	struct sockaddr_in seraddr = {0};
	seraddr.sin_family = AF_INET; 
	seraddr.sin_addr.s_addr = inet_addr(peeraddr.ip); 
	seraddr.sin_port = htons(peeraddr.port); 

	int ret = connect(n->fd(), (sockaddr *)&seraddr, sizeof(sockaddr));
	
	if( n->get_appid() < 0)
	{
		n->set_appid(get_appid());
	}
	
	if (ret < 0)
	{	
		if (errno != EINTR && errno != EINPROGRESS && errno != EISCONN)
		{
			printf_t("error: connect error(%d)\n", errno);
			n->set_status(kdisconnected);
			m_con_list.push_front(n);
			post_tcp_msg(n, ev_connect_fail);
			return -1;
		}	
		n->set_status(kconnecting);
		m_con_list.push_back(n);
	}
	else
	{
		n->set_status(kconnected);
		n->set_alive_time(time(NULL));
		m_con_list.push_back(n);			
		post_tcp_msg(n, ev_connect_ok);
	}

	return 0;
}
	

int server::handle_timer(evtime * e)
{
	if(e->id == ev_timer_active)
	{
		m_timer.add(e->id, e->interval, e->ptr);
	}
	else if(e->id == ev_con_keepalive)
	{	
		check_keepalive(e);
	}
	else if(e->id == ev_con_clear)
	{
		check_invalid_con(e);
	}
	else if(e->id == ev_con_connect)
	{
		start_connect(e);
	}
	else
	{	
		post_timer_msg(e);
	}
	return 0;
}


int server::handle_recv(connection * n)
{
	packet_buf * p_buf = n->get_recv_buf();		
	//et mode
	do 
	{
		int len = p_buf->len - p_buf->has;
		
		if(len <= 0){
			printf_t("error: packet too large\n");
			break;
		}
		
		int recv_bytes  = recv(n->fd(), p_buf->buf + p_buf->has, len, 0);
	
		if(recv_bytes < 0)
		{	
			if(errno != EINTR && errno != EAGAIN)
			{
				printf_t("error: recv error(%d %s) socket(%d)\n", errno, strerror(errno), n->fd());
				return -1;
			}
			return 0;
		}
		else if(recv_bytes == 0)
		{
			printf_t("warn : remote close error(%d %s) socket(%d)\n", errno, strerror(errno), n->fd());
			return -1;
		}
		else
		{
			p_buf->has += recv_bytes;
		}
		
		if(packet_dispatch(n) < 0)
		{
			return -1;
		}
		
		m_con_list.move_to_back(n);
			
		if(len != recv_bytes)
		{
			break;
		}
		
	} while (true);	
	return 0;
}


int server::handle_connect(connection * n)
{
	int error = 0;
	int len = sizeof(error);
	int ret = getsockopt(n->fd(), SOL_SOCKET, SO_ERROR, &error, (socklen_t*)&len);
	
	if(ret < 0 || error)
	{
		printf_t("error: connect fail socket(%d) ret(%d) error(%d %d)\n", n->fd(), ret, error, errno);
		return -1;
	}
	
	n->set_alive_time(time(NULL));
	n->set_status(kconnected);		
	m_con_list.move_to_back(n);
	post_tcp_msg(n, ev_connect_ok);
	
	printf_t("debug: connect ok socket(%d)\n", n->fd());
	
	return 0;
}

int server::handle_write(connection * n)
{
	if(n->get_status() == kconnecting)
	{
		return handle_connect(n);
	}
	else
	{
		return n->post_send();
	}
}

int server::handle_close(connection * n,  int reason)
{
	if(n->get_status() == kconnected)
	{
		n->set_status(kdisconnected);
		m_con_list.move_to_front(n);
		post_tcp_msg(n, ev_close, &reason, sizeof(reason));
	}
	else if(n->get_status() == kconnecting)
	{
		n->set_status(kdisconnected);
		m_con_list.move_to_front(n);
		post_tcp_msg(n, ev_connect_fail);
	}
	else
	{
		printf_t("warn : handle_close already closed socket(%d)\n", n->fd());
	}

	return 0;
}

int server::handle_accept()
{

	int fd = -1;
	struct sockaddr_in peeraddr; 
	int addrlen = sizeof(peeraddr); 
	int listenfd = m_listenfd;
	time_t cur = time(NULL);
	while((fd = accept(listenfd, (struct sockaddr *)&peeraddr, (socklen_t*)&addrlen)) >= 0)
	{	
			
		if (make_no_block(fd) < 0){
			printf_t("error: make_no_block error(%d)\n", errno);
			close(fd);
			continue;
		}
		
		int opt = 1024 * 128;
		if(setsockopt(fd, SOL_SOCKET ,SO_SNDBUF,(char *)&opt, sizeof(opt)) <0)
		{
			printf_t("error: set send buffer error(%d)\n", errno);
		}
	
		if(setsockopt(fd, SOL_SOCKET ,SO_RCVBUF,(char *)&opt, sizeof(opt)) < 0)
		{
			printf_t("error: set recv buffer error(%d)\n", errno);
		}
		
		//allocate appid		
		connection * n = new connection(m_epfd, fd);
		n->set_appid(get_appid());
		n->set_status(kconnected);
		n->set_peeraddr(peeraddr);
		n->set_alive_time(cur);
		m_con_list.push_back(n);
		post_tcp_msg(n, ev_accept);
		
		ipaddr paddr = n->get_peeraddr();
		
		printf_t("debug: accept from %s:%d  socket(%d)\n",paddr.ip, paddr.port, fd);
	}
	
	
	if (errno != EAGAIN && errno != ECONNABORTED && errno != EPROTO && errno != EINTR)
	{
		printf_t("error: net_accept.accept error\n");
		return -1;
	}
	
	return 0;
}

int server::set_keepalive(int timeout)
{
	m_keepalive_timeout = timeout < 0 ? 120 : timeout;
	return 0;
}

int server::init_log(const char* path, const char * name, int max_size)
{
	return log::instance()->init(path, name, max_size);
}

int server::init()
{
	//resource limit set 2015-1-23
	struct rlimit limit;
	limit.rlim_cur = 60000;
	limit.rlim_max = 60000;
	if(setrlimit(RLIMIT_NOFILE, &limit) < 0)
	{
		printf_t("error: setrlimit error(%d)\n", errno);
	}
	
	//ignore socket pipe 2015-1-23
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	if(sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGPIPE, &sa, 0) == -1) 
	{ 
		printf_t("error: sigaction error(%d)\n", errno);
	}
	
	//ignore thread pipe 2015-1-23
	sigset_t signal_mask;
	sigemptyset(&signal_mask);
	sigaddset(&signal_mask, SIGPIPE);
	if(pthread_sigmask(SIG_BLOCK, &signal_mask, NULL) < 0)
	{
		printf_t("error: pthread_sigmask error(%d)\n", errno);
	}

	//create epoll
	m_epfd = epoll_create (32000);
	if (m_epfd < 0) {	
		printf_t("error: epoll create error(%d)\n", errno);
		return -1;
	}
	
		//init timer
	if(m_timer.init() < 0)
	{
		printf_t("error: timer init fail\n");
		close(m_epfd);
		return -1;
	}
	
	//add system timer event
	m_timer.add(ev_timer_active,  3600 * 1000, NULL);
	m_timer.add(ev_con_keepalive, 30000, NULL);
	m_timer.add(ev_con_clear, 5000, NULL);
	
	return 0;
}


int server::create_tcp_server(ushort port, int reuse /*= 1*/){
	
	int listenfd = socket(AF_INET, SOCK_STREAM, 0);	
	if(listenfd < 0){
		printf_t("error: create listen socket error(%d)\n", errno);
		return -1;
	}
	
	if (make_no_block(listenfd) < 0){
		printf_t("error: fcnt getfl error(%d)\n", errno);
		close(listenfd);
		return -1;
	}
	
	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0){
		printf_t("error: setsockopt SO_REUSEADDR error(%d)\n", errno);
		close(listenfd);
		return -1;
	}
	
	// bind & listen    
	sockaddr_in sin ;         
	memset(&sin, 0, sizeof(struct sockaddr_in));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = INADDR_ANY;
	
	if(bind(listenfd, (const sockaddr*)&sin, sizeof(sin)) < 0){
		printf_t("error: socket bind error(%d)\n", errno);
		close(listenfd);
		return -1;
	}
	
	if(listen(listenfd, SOMAXCONN) < 0){
		printf_t("error: listen error(%d)\n", errno);
		close(listenfd);
		return -1;
	}

	//add listen fd
	m_listenfd = listenfd;
	m_listen_port = port;
	
	struct epoll_event ev;
	ev.data.ptr = &m_listenfd;
	ev.events =  EPOLLIN | EPOLLET | EPOLLPRI;
	if (epoll_ctl (m_epfd, EPOLL_CTL_ADD, m_listenfd, &ev) < 0) {
		printf_t("error: epoll EPOLL_CTL_ADD error(%d)\n", errno);
		close(m_listenfd);
		return -1;
	}

	return 0;
}

int server::loop()
{
	prctl(PR_SET_NAME, "epoll");
	if(!m_app_num)
	{
		return -1;
	}
	return run();
}

int server::add_timer(int id, int interval, int appid, void * context)
{
	if(appid < 0)
	{
		return -1;
	}
	return m_timer.add(id, interval, context, appid) ? 0 : -1;
}

int server::add_abs_timer(int id, int year, int mon, int day, 
						  int hour, int min, int sec, int appid,  void * context /* = NULL */){

	struct tm tnow = {0};
    tnow.tm_year = year - 1900;
    tnow.tm_mon  = mon - 1;
    tnow.tm_mday = day;
    tnow.tm_hour = hour;
    tnow.tm_min  = min;
    tnow.tm_sec  = sec;

	time_t tsecs = mktime(&tnow);
	if(tsecs == (time_t)(-1)){
		return -1;
	}

	time_t curtime;
	curtime = time(NULL);
	int interval = (int)difftime(tsecs, curtime) * 1000;
	return m_timer.add(id, interval, context, appid) ? 0 : -1;
}

int server::register_app(app * a, int msg_count, const char * name)
{
	if(!a || m_app_num >= max_app_num){
		return -1;
	}

	if(a->create(m_app_num, msg_count, name) < 0)
	{
		delete a;
		return -1;
	}
	
	m_apps[m_app_num] = a;

	return  m_app_num++;
}

int server::post_connect(const char * ip, ushort port, int delay, int appid , void * context)
{
	if(delay < 0)
	{
		printf_t("error: delay(%d) <= 0\n", delay);
		return -1;
	}
	
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1)
	{	
		printf_t("error: post_connect create socket error(%d)\n", errno);
		return -1;
	}
	
	if(make_no_block(fd) < 0)
	{
		close(fd);
		printf_t("error: post_connect make_no_block error(%d)\n", errno);
		return -1;
	}
	
	ipaddr peeraddr = {{0}, port};
	strncpy(peeraddr.ip, ip, sizeof(peeraddr.ip) - 1);
	
	connection * n = new connection(m_epfd, fd);
	n->set_peeraddr(peeraddr);
	n->set_context(context);
	n->set_appid(appid);
		
	if(!m_timer.add(ev_con_connect, delay, n))
	{
		delete n;
		return -1;
	}
	return 0;
}


int server::stop()
{
	
	if(m_epfd >= 0){
	
		close(m_epfd);
		m_epfd = -1;
	}
	
	if(m_listenfd >= 0){
		close(m_listenfd);
		m_listenfd = -1;
	}
	
	m_timer.release();
	
	for(int k = 0; k < m_app_num; k++)
	{
		delete m_apps[k];
	}
	m_app_num = 0;

	return 0;
}

