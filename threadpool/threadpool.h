#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
        /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
        threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
        ~threadpool();
        bool append(T *request, int state);
        bool append_p(T *request);

private:
        /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
        static void *worker(void *arg);
        void run();
private:
        int m_thread_number;          //线程池中的线程数
        int m_max_requests;           //请求队列中允许的最大请求数
        pthread_t *m_threads;         //描述线程池的数组，其大小为m_thread_number
        std::list<T *> m_workqueue;   //请求队列
        locker m_queuelocker;         //保护请求队列的互斥锁
        sem m_queuestat;              //是否有任务需要处理
        connection_pool *m_connPool;  //数据库
        int m_actor_model;            //模型切换（这个切换是指 Reactor/Proactor）
};

template <typename T>
// 线程池构造函数
// m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) 
: m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{       
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception(); 
    for (int i = 0; i < thread_number; ++i)   //   m_thread_num = 8 + 
    {  
            //函数原型中的第三个参数，为函数指针，指向处理线程函数的地址
            //若线程函数为类成员函数，
            //则this指针会作为默认的参数被传进函数中，从而和线程函数参数(void*)不能匹配，不能通过编译
            //静态成员函数就没有这个问题，因为里面没有this指针 
            
            //创建成功应该返回 0，如果线程池在线程创建阶段就失败，那就应该关闭线程池了 +
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        } 
        //主要是将线程属性更改为 unjoinable，便于资源的释放  +
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}


template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

//将request 加入队列
//reactor模式下的请求入队 +
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    //读写事件
    request->m_state = state;
    m_workqueue.push_back(request);   //将request加入队列
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

/*
当epoll检测到端口有事件激活时，即将该事件放入请求队列中（注意互斥），等待工作线程处理
*/

//proactor模式下的请求入队 +
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();   // 唤醒信号
    return true;
}


template <typename T>
//调用时 *arg是this！
//所以该操作其实是获取threadpool对象地址
//工作线程:pthread_create时就调用了它
void *threadpool<T>::worker(void *arg)
{    
    threadpool *pool = (threadpool *)arg;   
    //线程池中每一个线程创建时都会调用run()，睡眠在队列中 +
    pool->run();
    return pool;
}


/*
 * 可以看做是一个回环事件，一直等待 m_queuestat() 信号变量post，即新任务进入请求队列，这时请求队列中取出一个任务进行处理：
 */

//线程池中的所有线程都睡眠，等待请求队列中新增任务 +
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        pthread_t tid = 0;
        tid = pthread_self();
        std::cout << "线程  =  " << tid <<  std::endl;
        std::cout << "阻塞在这里" << std::endl;
        m_queuestat.wait();  //多线程的时候会阻塞
        m_queuelocker.lock();  // 加锁
        std::cout << "加锁之后" << std::endl;
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();    //请求队列的最前面一个
        m_workqueue.pop_front();   //从队列前面删除
        m_queuelocker.unlock();   // 解锁
        if (!request)
                continue;
        //        ......线程开始进行任务处理  +   //Reactor模式
        if (1 == m_actor_model)
        {
            //  IO事件类型：0为读
            if (0 == request->m_state)
            {       
                if (request->read_once())
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {   // IO事件类型 1为写
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        //default:Proactor，线程池不需要进行数据读取，而是直接开始业务处理
        //之前的操作已经将数据读取到http的read和write的buffer中了
        else
        {
            std::cout << "threadpool" <<std::endl;
            connectionRAII mysqlcon(&request->mysql, m_connPool);  // 应该是初始化一个数据库线程
            request->process();   //http_conn 中
        }
    }
}
#endif
