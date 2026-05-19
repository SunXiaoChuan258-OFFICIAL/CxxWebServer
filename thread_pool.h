#pragma once

#include <atomic>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include <vector>
#include "lock.h"

template<typename T>
concept Workable=requires(T t){//要求所有任务类T都要实现process方法
    t.process();
};



template<Workable T>
class threadpool{
private:
    int m_thread_number;
    int m_max_requests;
    std::vector<pthread_t> m_threads;
    

   
    std::vector<T*> m_workqueue;//循环工作队列，防止vector自动扩容，同时有能享受自动内存管理，                   
    std::atomic<size_t> m_head,m_tail;//头尾的append,pop是要互斥访问的，head和tail用atomic包装
    size_t m_queue_size;//队列大小


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
    
    m_workqueue(max_requests),//环形数组初始化为最大,防止扩容
    m_head(0),//头尾指针初始化
    m_tail(0),
    m_queue_size(max_requests),

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
    
    size_t cur_tail=m_tail.load(std::memory_order_relaxed);//原子化获得当前m_tail插入点,只有主线程一个生产者，所以可以用relaxed内存序
    size_t next_tail=(cur_tail+1)%m_queue_size;//计算下一个插入点

    if(next_tail==m_head.load(std::memory_order_acquire)){//队列判满，舍弃一个单元(m_tail+1)%m_queue_size==m_head
        return false;
    }

    m_workqueue[cur_tail]=request;
    m_tail.store(next_tail,std::memory_order_release);//m_tail移动一格
 

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
        m_queuestat.wait();//P操作，获取资源
        
        size_t cur_head=m_head.fetch_add(1,std::memory_order_acq_rel)%m_queue_size;//获取队头,fetch_add相当于原子化pop
        size_t cur_tail=m_tail.load(std::memory_order_acquire);
        
        if(cur_head==cur_tail){//队列为空，退出继续尝试获取资源,理论上信号量确保了不会为空
            m_head.fetch_sub(1,std::memory_order_acq_rel)%m_queue_size;//为空时要回退
            continue;
        }

        T* request=m_workqueue[cur_head];
       

        if(!request){
            continue;
        }


        request->process();//request封装了用户业务逻辑，必须实现process处理函数





    }



}









