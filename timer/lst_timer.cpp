#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}


//添加定时器
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    //定时器中是按照expire从小到大排序
    //如果新的定时器    超时时间   小于  当前头部结点
    //直接将当前定时器结点作为头部结点
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}

//调整定时器，任务发生变化时，调整定时器在链表中的位置
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    //  被调整的定时器在链表尾部
    //  or 定时器超时值仍然小于下一个定时器超时值，不调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    //被调整定时器在内部，将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}


void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)
        {
            break;
        }
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

//怎么加入新的定时器呢？
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
        util_timer *prev = lst_head;
        util_timer *tmp = prev->next;

        //从双向链表中找到该定时器应该放置的位置
        //即遍历一遍双向链表找到对应的位置
        //时间复杂度太高O(n)
        //这里可以考虑使用C++11的优先队列实现定时器-----TODO

        while (tmp)
        {
            if (timer->expire < tmp->expire)
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
         //遍历完发现，目标定时器需要放到尾结点处
        if (!tmp)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
}

// TIMESLOT = 5;     
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;  
    else  //TRIGMode == 0
        event.events = EPOLLIN | EPOLLRDHUP;  //可读或者挂断

    //如果对描述符socket注册了EPOLLONESHOT事件，
    //那么操作系统最多触发其上注册的一个可读、可写或者异常事件，且只触发一次 +
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
//创建sigaction结构体变量，设置信号函数 +
void Utils::sig_handler(int sig)
{
        //为保证函数的可重入性，保留原来的errno
        //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据 +
        int save_errno = errno;
        int msg = sig;
        // 将信号值从管道写端写入，传输 字符类型，而非整型
        send(u_pipefd[1], (char *)&msg, 1, 0);
        errno = save_errno;
        //信号处理函数中仅仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响。 +

}

// 默认restart = true 
//设置信号函数               传入了 handler函数    
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
        //创建sigaction结构体变量
        struct sigaction sa;
        memset(&sa, '\0', sizeof(sa));

        //信号处理函数中仅仅发送信号值，不做对应逻辑处理
        sa.sa_handler = handler;
        if (restart)
                sa.sa_flags |= SA_RESTART;

        //将所有信号添加到信号集中
        sigfillset(&sa.sa_mask);

        //执行sigaction函数 
        assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;  //应该是赋初值 静态成员要初始化
int Utils::u_epollfd = 0;

class Utils; 
//定时器回调函数:从内核事件表删除事件，关闭文件描述符，释放连接资源
void cb_func(client_data *user_data)
{
        std::cout << "cb_func()函数" << std::endl;
        //删除非活动连接在socket上的注册事件
        std::cout << "user_data->sockfd = " << user_data->sockfd << std::endl;
        epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
        assert(user_data);
        //删除非活动连接在socket上的注册事件
        close(user_data->sockfd);
        //减少连接数
        http_conn::m_user_count--;
}
