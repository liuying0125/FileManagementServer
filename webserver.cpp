#include "webserver.h"

WebServer::WebServer()
{
        //http_conn类对象
        users = new http_conn[MAX_FD];

        //root文件夹路径
        char server_path[200];
        getcwd(server_path, 200);
        char root[6] = "/root";
        m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);  // 加1是因为 \0 
        strcpy(m_root, server_path);
        strcat(m_root, root);

        //定时器
        users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{       
        close(m_epollfd);
        close(m_listenfd);
        close(m_pipefd[1]);
        close(m_pipefd[0]);
        delete[] users;
        delete[] users_timer;
        delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
        m_port = port;
        m_user = user;
        m_passWord = passWord;
        m_databaseName = databaseName;
        m_sql_num = sql_num;
        m_thread_num = thread_num;
        m_log_write = log_write;
        m_OPT_LINGER = opt_linger;
        m_TRIGMode = trigmode;  //初始时候是0
        m_close_log = close_log;
        m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
        //LT + LT
        if (0 == m_TRIGMode)
        {
                m_LISTENTrigmode = 0;
                m_CONNTrigmode = 0;
        }
        //LT + ET
        else if (1 == m_TRIGMode)
        {
                m_LISTENTrigmode = 0;
                m_CONNTrigmode = 1;
        }
         //ET + LT
        else if (2 == m_TRIGMode)
        {
                m_LISTENTrigmode = 1;
                m_CONNTrigmode = 0;
        }
         //ET + ET
        else if (3 == m_TRIGMode)
        {
                m_LISTENTrigmode = 1;
                m_CONNTrigmode = 1;
        }
}

void WebServer::log_write()
{
        if (0 == m_close_log)
        {
                std::cout << "m_log_write = "  << m_log_write << std::endl;
                //初始化日志
                if (1 == m_log_write)
                        Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
                else
                        Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
        }
}

void WebServer::sql_pool()
{
        //初始化数据库连接池
        m_connPool = connection_pool::GetInstance();
        m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

        //初始化数据库读取表
        users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池处理的对象就是http类的GET与POST请求。
    //线程池                          ( int actor_model, connection_pool *connPool, int thread_number, int max_requests) 
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    //网络编程基础步骤    创建监听socket文件描述符 +
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    //  std::cout << "m_listenfd  =  " << m_listenfd << std::endl;  
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
            struct linger tmp = {0, 1};
            setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
            struct linger tmp = {1, 1};
            setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    //   创建监听socket的TCP/IP的IPV4 socket地址 +
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    //      SO_REUSEADDR 允许端口被重复使用 +
    //  
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    //      绑定socket和它的地址 +
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    //      创建监听队列以存放待处理的客户连接，在这些客户连接被accept()之前 
    ret = listen(m_listenfd, 5);  //排队队列是 5
    assert(ret >= 0);

    utils.init(TIMESLOT);  // TIMSLOT = 5

    //epoll创建内核事件表     10000
    epoll_event events[MAX_EVENT_NUMBER];
    //size参数现在并不起作用，只是给内核一个提示，告诉内核应该如何为内部数据结构划分初始大小。
    m_epollfd = epoll_create(5);  //size没有意义 
    assert(m_epollfd != -1);
    //  将  m_listenfd  上的EPOLLIN和EPOLLET事件注册到epollfd指示的epoll内核事件中 
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;


    //socketpair函数能够创建一对套接字进行通信，项目中使用管道通信 
//                                 协议族           协议     protocol表示类型，只能为0 
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    //    sv[2]表示套节字柄对，该两个句柄作用相同，均能进行读写双向操作 +
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);  //对文件描述符设置非阻塞
    //设置管道读端为ET非阻塞，并添加到epoll内核事件表
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);
    //传递给主循环的信号值，这里只关注 SIGALRM 和 SIGTERM  
    utils.addsig(SIGPIPE, SIG_IGN);   //设置 SIG_IGN信号处理函数 (忽视信号)
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);


    //每隔TIMESLOT时间触发SIGALRM信号 +
    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{       
        //  将connfd注册到内核事件表中  +
        users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

        //初始化client_data数据
        //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
        users_timer[connfd].address = client_address;
        users_timer[connfd].sockfd = connfd;
        util_timer *timer = new util_timer;
        timer->user_data = &users_timer[connfd];
        timer->cb_func = cb_func;
        time_t cur = time(NULL);
        timer->expire = cur + 3 * TIMESLOT;
        users_timer[connfd].timer = timer;
        utils.m_timer_lst.add_timer(timer);
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
        time_t cur = time(NULL);
        timer->expire = cur + 3 * TIMESLOT;
        utils.m_timer_lst.adjust_timer(timer);

        LOG_INFO("%s", "adjust timer once");
}

//删除定时器节点，关闭连接
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
        timer->cb_func(&users_timer[sockfd]);
        if (timer)
        {
            utils.m_timer_lst.del_timer(timer);
        }

        LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

//http 处理用户数据
bool WebServer::dealclinetdata()
{
        struct sockaddr_in client_address;
        socklen_t client_addrlength = sizeof(client_address);
        if (0 == m_LISTENTrigmode)
        {       //LT水平触发 + 
                std::cout << "LT水平触发" << std::endl;
                int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                std::cout << "connfd = " << connfd << std::endl;
                if (connfd < 0)
                {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        return false;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                        utils.show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        return false;
                }
                // 并将connfd注册到内核事件表中  +
                timer(connfd, client_address);
        }       
        else
        {       //ET非阻塞边缘触发 +   //边缘触发需要  一直accept直到为空
                std::cout << "ET非阻塞边缘触发" << std::endl;
                while (1)
                {       //      accept()返回一个新的socket文件描述符用于send()和recv()  +
                        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                        if (connfd < 0)
                        {
                                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                                break;
                        }
                        if (http_conn::m_user_count >= MAX_FD)
                        {
                                utils.show_error(connfd, "Internal server busy");
                                LOG_ERROR("%s", "Internal server busy");
                                break;
                        }
                        timer(connfd, client_address);
                }
                return false;
        }
        return true;
}

//处理信号函数
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
        int ret = 0;
        int sig;
        char signals[1024]; 
        //从管道读端读出信号值，成功返回字节数，失败返回-1
        //正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
        ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
        if (ret == -1)
        {
            return false;
        }
        else if (ret == 0)
        {
            return false;
        }
        else
        {    //处理信号值对应的逻辑  +   
            for (int i = 0; i < ret; ++i)
            {
                switch (signals[i]){
                    case SIGALRM:{
                        //  std::cout << "SIGALRM信号 "  << std::endl;
                        timeout = true;
                        break;
                    }
                    case SIGTERM:{
                        std::cout << "SIGTERM信号 "  << std::endl;
                        stop_server = true;
                        break;
                    }
                }
            }
        }
        return true;
}

//处理客户连接上接收到的数据
void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer; 
    //reactor
    if (1 == m_actormodel)
    {
        if (timer){
            adjust_timer(timer);
        } 
        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        std::cout << "proactor" << std::endl;
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer){
                adjust_timer(timer);
            }
        }
        else{
            deal_timer(timer, sockfd);
        }
    }
}


//写操作
void WebServer::dealwithwrite(int sockfd)
{
        util_timer *timer = users_timer[sockfd].timer;
        //reactor
        if (1 == m_actormodel)
        {
            if (timer){
                    adjust_timer(timer);
            }

            m_pool->append(users + sockfd, 1);

            while (true)
            {
                if (1 == users[sockfd].improv)
                {
                    if (1 == users[sockfd].timer_flag)
                    {
                        deal_timer(timer, sockfd);
                        users[sockfd].timer_flag = 0;
                    }
                    users[sockfd].improv = 0;
                    break;
                }
            }
        }
        //proactor
        else
        {      
            std::cout << "dealwithwrite中的proactor"<<std::endl;
            if (users[sockfd].write())
            {
                LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

                if (timer)
                {
                    adjust_timer(timer);
                }
            }
            else
            {
                deal_timer(timer, sockfd);
            }
        }
}

// 服务器运行
void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
            // 等待所监控文件描述符上有事件的产生 + 
            // 主线程调用epoll_wait等待一组文件描述符上的事件，并将当前所有就绪的epoll_event复制到events数组中 +
            //epoll返回就绪的文件描述符个数 +
            int number = epoll_wait(m_epollfd,  , MAX_EVENT_NUMBER, -1);
            if (number < 0 && errno != EINTR)
            {
                    LOG_ERROR("%s", "epoll failure");
                    break;
            }

            for (int i = 0; i < number; i++)
            {
                    int sockfd = events[i].data.fd;
                    std::cout << "主循环中 sockfd = " << sockfd << std::endl;
                    //处理新到的客户连接   m_listenfd = 12
                    if (sockfd == m_listenfd)
                    {       
                            std::cout << "处理新的客户链接 sockfd = m_listenfd = " << sockfd << std::endl;
                            bool flag = dealclinetdata();
                            if (false == flag)
                                continue;
                    }
            
                    //  处理异常事件
                    //  对应的文件描述符        对端断开连接       挂断      错误
                    else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                    {
                            //服务器端关闭连接，移除对应的定时器
                            util_timer *timer = users_timer[sockfd].timer;
                            deal_timer(timer, sockfd);
                    }
                    //处理信号              //管道读端对应文件描述符发生读事件 +
                    else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
                    {
                            //没有接入 number = 1 的时候
                            std::cout << "执行 ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) " << std::endl;
                            bool flag = dealwithsignal(timeout, stop_server);  //读取信号
                            if (false == flag)
                                    LOG_ERROR("%s", "dealclientdata failure");
                    }
                    //处理客户连接上接收到的数据
                    //当这一sockfd上有可读事件时，epoll_wait通知主线程  +
                    else if (events[i].events & EPOLLIN)   // EPOLLIN 对应的文件描述符可以读 +
                    {
                            // std::cout << "events[i].events & EPOLLIN" << std::endl;
                            dealwithread(sockfd);
                    }
                    //当这一sockfd上有可写事件时，epoll_wait通知主线程。dealwithwrite往socket上写入服务器处理客户请求的结果 + 
                    else if (events[i].events & EPOLLOUT)   //对应的文件描述符可以写 +
                    {
                            std::cout << "events[i].events & EPOLLOUT" << std::endl;
                            dealwithwrite(sockfd);
                    }
            }


            if (timeout)  //如果上面读取的信号是 true 那么执行下面的代码
            {
                    std::cout << "读取信号是true" << std::endl;
                    utils.timer_handler();

                    LOG_INFO("%s", "timer tick");

                    timeout = false;
            }
    }
}