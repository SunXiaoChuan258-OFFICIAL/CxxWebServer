#pragma once

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include "lock.h"

class Http_Conn{
public:
    static constexpr int FILENAME_LEN=200;   //最大文件名长度
    static constexpr int READ_BUFFER_SIZE=2048;  //读写缓冲大小
    static constexpr int WRITE_BUFFER_SIZE=1024;

    enum class METHOD: int{   //client requests method
        GET=0,POST,HEAD,PUT,DELETE,TRACE,OPTIONS,CONNECT,PATCH
    };

    enum class CHECK_STATE:int{ //主状态机
      CHECK_STATE_REQUESTLINE=0,
      CHECK_STATE_HEADER,
      CHECK_STATE_CONTENT
    };

    enum class LINE_STATUS:int{  //从状态机
        LINE_OK=0,LINE_BAD,LINE_OPEN
    };


    enum class HTTP_CODE:int{  //parse results of server
        NO_REQUEST=0,GET_REQUEST,BAD_REQUEST,NO_RESOURCE,FORBIDDEN_REQUEST,FILE_REQUEST,
        INTERNAL_ERROR,CLOSED_CONNECTION
    };


public:
    Http_Conn(){}
    ~Http_Conn(){}
   
public:
    void init(int sockfd,const sockaddr_in& addr);//初始化http连接的sockfd和客户网络地址
    void close_conn(bool read_close=true);//关闭http连接
    
    //处理客户请求线程的run()方法中在加锁获取request客户逻辑后会调用process()方法处理逻辑
    void process();

    //非阻塞读
    bool read();//循环读取内核缓冲区数据

    //非阻塞写
    bool write();

    
private:
    void init();//public init初始化连接时调用的重载函数

    HTTP_CODE process_read();//parse requestline

    bool process_write(HTTP_CODE ret);//return parse result to client

    //process_read用来解析http请求
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text); 
    HTTP_CODE do_request();
    char* get_line(){return m_read_buf+m_start_line;}
    LINE_STATUS parse_line();

    void unmap();
    bool add_response(const char* format,...);

    bool add_status_line(int status,const char* titile);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char* content);






    
public:
    //所有Http_Conn都要共享同一个m_epollfd,同时统计用户数量
    static int m_epollfd;
    static int m_user_count;




private:
    int m_sockfd;  //连接基本信息，sockfd和sockaddr_in
    sockaddr_in m_address;

    char m_read_buf[READ_BUFFER_SIZE];//读缓冲，m_read_idx，m_checked_idx,m_start_line
    int m_read_idx;
    int m_checked_idx;
    int m_start_line;

    char m_write_buf[WRITE_BUFFER_SIZE];//写缓冲,m_write_idx
    int m_write_idx;

    CHECK_STATE m_check_state;  //主状态机
    METHOD m_method;  //请求方法

    char m_real_file[FILENAME_LEN];//完整文件路径，等于doc_root+m_url,doc_root是网站的根目录，对用户不可见
    char* m_url;//用户请求文件名
    char* m_version; //协议版本，这里只支持http1.1

    char* m_host;  
    long m_content_length;//消息体的长度
    bool m_linger;//控制长短连接

    //文件mmap到内存中的起始位置
    char* m_file_address;

    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;




};