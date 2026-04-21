#pragma once

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include <vector>
#include "lock.h"

template<typename T>
concept Workable=requires(T t){
    t.process();
};


template<Workable T>
class threadpool{
private:
    int m_thread_number;
    int m_max_requests;
    std::vector<pthread_t> m_threads;
    std::list<T*> m_workqueue;

    locker m_queuelock;
    sem m_queuestat;
    bool m_stop;

public:
    threadpool(int thread_number=8,int max_requests=10000);
    ~threadpool();

    bool append(T* request);//添加任务


private:
    static void* worker(void* arg);//工作线程运行的函数，不断取出任务并执行
    void run(); 


};



template<Workable T>
threadpool<T>::threadpool(int thread_number,int max_requests)
    :m_thread_number(thread_number),
    m_max_requests(max_requests),
    m_stop(false),
    m_threads(thread_number)
{ 
    //初始化线程数量和请求数量，m_stop标志设为false,线程描述vector数组初始化
    if(thread_number<=0 || max_requests<=0){
        throw std::exception();
    }

    

    for(int i=0;i<m_thread_number;++i){//创建线程后立即detach
        if(pthread_create(&m_threads[i],NULL,worker,this)!=0){
            throw std::exception();
        }

        if(pthread_detach(m_threads[i])){
            throw std::exception();
        }

    }


}


template<Workable T>
threadpool<T>::~threadpool(){
    m_stop=true;  //修改停止标记位
}


template<Workable T>
bool threadpool<T>::append(T* request){
    m_queuelock.lock();
    if(m_workqueue.size()>=m_max_requests){//数量超限，添加失败
        m_queuelock.unlock();
        return false;
    }

    m_workqueue.push_back(request);//添加请求到队列
    m_queuelock.unlock();//解锁队列锁
    m_queuestat.post();//更新队列状态，request数量增加

    return true;


}

template<Workable T>
void* threadpool<T>::worker(void* arg){
    threadpool<T>* pool=static_cast<threadpool<T>*>(arg);
    pool->run();
    return pool;
}


template<Workable T>
void threadpool<T>::run(){
    
    while(!m_stop){
        m_queuestat.wait();//P操作，获取资源后立即上锁
        m_queuelock.lock();

        if(m_workqueue.empty()){
            m_queuelock.unlock();
            continue;
        }

        T* request=m_workqueue.front();
        m_workqueue.pop_front();

        m_queuelock.unlock();

        if(!request){
            continue;
        }


        request->process();//request封装了用户业务逻辑，必须实现process处理函数





    }



}






