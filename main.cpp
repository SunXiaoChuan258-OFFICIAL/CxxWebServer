#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cassert>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <memory>
#include <vector>


#include "lock.h"
#include "thread_pool.h"
#include "http_conn.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000


extern int addfd(int epollfd,int fd,bool one_shot);//外部声明
extern int removefd(int epollfd,int fd);



//注册信号处理机制
void addsig(int sig,void(handler)(int),bool restart=true){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));

    sa.sa_handler=handler;
    if(restart){
        sa.sa_flags|=SA_RESTART;    
    }

    sigfillset(&sa.sa_mask);

    assert(sigaction(sig,&sa,NULL)!=-1);

}



void show_error(int connfd,const char* info){//向对端报错，并关闭连接
    printf("%s\n",info);
    send(connfd,info,strlen(info),0);
    close(connfd);
}


int main(int argc,char* argv[]){
    if(argc<=2){
        printf("usage: %s ipaddress portnumber\n",basename(argv[0]));
    }


    const char* ip=argv[1];
    int port=atoi(argv[2]);
    
    addsig(SIGPIPE,SIG_IGN);//忽略管道报错信号

    std::unique_ptr<threadpool<Http_Conn>> pool;

    try{
        //创建一个操作Http_Conn的线程池，并用unique_ptr pool指向他
        printf("creating thread_pool...\n");
        pool=std::make_unique<threadpool<Http_Conn>>();

    }

    catch(...){
        return 1;
    }


    printf("thread_pool_built!\n");
    std::vector<Http_Conn> users(MAX_FD);//以空间换时间，为每个可能的connfd准备一个Http_Conn
    int user_count=0;//记录用户数量，与m_user_count不同

    
    
    //经典listenfd创建套路，socket和address_in,bind,listen,只是多设了一个SO_LINGER
    int listenfd=socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd>=0);

    struct linger tmp={1,0};
    setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    

    int ret=0;
    struct sockaddr_in address;
    memset(&address,'\0',sizeof(address));
    address.sin_family=AF_INET;
    inet_pton(AF_INET,ip,&address.sin_addr);
    address.sin_port=htons(port);


    ret=bind(listenfd,(struct sockaddr*)&address,sizeof(address));
    assert(ret!=-1);

    ret=listen(listenfd,5);
    assert(ret>=0);




    std::vector<epoll_event> events(MAX_EVENT_NUMBER);  //epoll_event数组
    
    int epollfd=epoll_create(5);
    assert(epollfd>=0);

    addfd(epollfd,listenfd,false);//主线程控制listenfd，不存在竞争，关闭EPOLLONESHOT

    Http_Conn::m_epollfd=epollfd;//任务类与主线程共享epollfd

    while(true){
        int number=epoll_wait(epollfd,events.data(),MAX_EVENT_NUMBER,-1);
        if(number<0 && errno!=EINTR){
            printf("epoll failure\n");
        }


        for(int i=0;i<number;++i){
            int sockfd=events[i].data.fd;
            if(sockfd==listenfd){
                struct sockaddr_in client_addr;
                socklen_t client_addr_len=sizeof(client_addr);

                int connfd=accept(listenfd,(struct sockaddr*)&client_addr,&client_addr_len);
                if(connfd<0){
                    printf("errno is %d\n",errno);
                    continue;
                }

                if(Http_Conn::m_user_count>=MAX_FD){//服务器繁忙
                    show_error(connfd,"sry,server too busy,please try again\n");//向对端报错，并关闭连接
                    continue;
                }


                printf("a new user joined\n");
                users[connfd].init(connfd,client_addr);//客户连接初始化



                

            }


            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                printf("a user left\n");
                users[sockfd].close_conn();
            }


            else if(events[i].events & EPOLLIN){
                if(users[sockfd].read()){//如果能正常把内核缓冲区读空
                    pool->append(dynamic_cast<Http_Conn*>((&users[sockfd])));//就把连接加入到请求队列中
                }

                else {//否则关闭连接
                    users[sockfd].close_conn();
                }

            }


            else if(events[i].events & EPOLLOUT){
                if(!users[sockfd].write()) users[sockfd].close_conn();
                printf("echo sent successfully\n");

            }

            else {

            }



        }


    }

    close(epollfd);
    close(listenfd);
    return 0;



}









