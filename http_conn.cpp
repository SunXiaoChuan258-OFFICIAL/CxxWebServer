#include "http_conn.h"

constexpr const char* ok_200_title="OK";  //  HTTP/1.1 404 NOT FOUND(title) --ContentLength--ContentType --error_404_form(form)
constexpr const char* error_400_title="BAD_REQUEST";
constexpr const char* error_400_form="Your request has bad syntax or is inherently impossible to satisfy\n";
constexpr const char* error_403_title="Forbidden";
constexpr const char* error_403_form="You dont have the permission to get the file from the server\n";
constexpr const char* error_404_title="Not Found";
constexpr const char* error_404_form="The requested file was not found on the server\n";
constexpr const char* error_500_title="Internal error";
constexpr const char* error_500_form="There is an unusual problem serving the requested file\n";

constexpr const char* doc_root="/var/www/html";


//以下为常规操作:1.setnonblock 2.addfd 3.removefd 4.modfd



int setnonblock(int fd){
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}


void addfd(int epollfd,int fd,bool one_shot){
    epoll_event event;
    event.data.fd=fd;
    event.events=EPOLLIN | EPOLLRDHUP | EPOLLET;
    
    if(one_shot){
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblock(fd);

}

void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,NULL);
    close(fd);

}


void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd=fd;
    event.events=ev | EPOLLRDHUP | EPOLLET | EPOLLONESHOT;
    //在oneshot触发后内核会移除epollfd上socket对应的事件，其他线程无法感知到该socket，实现了互斥访问
    //但是处理完后要重新添加EPOLLONESHOT事件                                                    


    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);

}


int Http_Conn::m_user_count=0;//用户数量初始化为0
int Http_Conn::m_epollfd=-1;  //epollfd初始化为-1


void Http_Conn::close_conn(bool real_close){
    if(real_close && m_sockfd!=-1){
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;//m_sockfd标记为-1
        m_user_count--;//用户数量减少

    }

}



void Http_Conn::init(int sockfd,const sockaddr_in& address){
    m_sockfd=sockfd;
    m_address=address;

    int reuse=1;
    setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));//设置为地址复用防止timewait
    addfd(m_epollfd,m_sockfd,true);//注册fd上的读事件，关闭事件，ET，EPOLLONESHOT
    m_user_count++;//用户数量+1
    //此处为通用处理，不管是处理http连接还是什么连接
    //init用来初始化用户逻辑


    init();//调用重载函数

}


void Http_Conn::init(){
    m_check_state=CHECK_STATE::CHECK_STATE_REQUESTLINE;  //进入请求行处理
    m_linger=false;  //关闭长连接

    m_method=METHOD::GET;  //初始化为GET头，网址(相对地址)设为NULL，其他的如版本号，消息体长度都类似
    m_url=NULL;
    m_version=NULL;

    m_content_length=0;
    m_host=NULL;

    m_start_line=0;
    m_checked_idx=0;
    m_read_idx=0;
    m_write_idx=0;



    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);//读写缓冲和文件名缓冲初始化


}


Http_Conn::LINE_STATUS Http_Conn::parse_line(){//从状态机，如果行完整会立即返回LINE_OK,不完整返回LINE_OPEN,语法错误返回LINE_BAD
    char tmp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx){
        tmp=m_read_buf[m_checked_idx];
        if(tmp=='\r'){
            if(m_checked_idx+1==m_read_idx) return LINE_STATUS::LINE_OPEN;
            else if(m_read_buf[m_checked_idx+1]=='\n'){
                m_read_buf[m_checked_idx++]='\0';   //将\r\n设置为终止零
                m_read_buf[m_checked_idx++]='\0';  //此时m_checked_idx一定指向下一行起始
                return LINE_STATUS::LINE_OK;

            }
            else return LINE_STATUS::LINE_BAD;

        }

        else if(tmp=='\n'){
            if(m_checked_idx>1 && m_read_buf[m_checked_idx-1]=='\r'){  //\n为开头时，确保缓冲区前一个是'\r'，对应前面'\r'结尾的类型
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_STATUS::LINE_OK;

            }
            else return LINE_STATUS::LINE_BAD;


        }


    }
    return LINE_STATUS::LINE_OPEN;//未检测到\r\n


}


bool Http_Conn::read(){  //循环读取数据直到内核缓冲区为空
    if(m_read_idx>=READ_BUFFER_SIZE){
        return false; //读缓冲已满
    }

    int bytes_read=0;
    while(true){
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){
                break;//如果是内核读缓冲已空导致的异常直接停止读取
            }
            return false;

        }
        else if(bytes_read==0){//对端关闭连接
            return false;
        }

        m_read_idx+=bytes_read;//更新m_read_idx指针


    }
    return true;

}


Http_Conn::HTTP_CODE Http_Conn::parse_request_line(char* text){//请求行解析，主要使用strpbrk,strspn,strchr
    using HC=HTTP_CODE;
    using MD=METHOD;
    using CS=CHECK_STATE;
    
    m_url=strpbrk(text," \t");  //strpbrk用来找到第一个空格或者制表符，
    if(!m_url){
        return HC::BAD_REQUEST;
    }

    *m_url++='\0';//并用\0分割GET和url

    char* method=text;
    if(strcasecmp(method,"GET")==0){
        m_method=MD::GET;
    }
    else {
        return HC::BAD_REQUEST;//只支持GET方法
    }



    m_url+=strspn(m_url," \t"); //跳过空格或制表符(tab)

    m_version=strpbrk(m_url," \t");
    if(!m_version){
        return HC::BAD_REQUEST;
    }
    *m_version++='\0';

    m_version+=strspn(m_version," \t");//LINE末尾被parse_line设置为了'\0'
    if(strcasecmp(m_version,"HTTP/1.1")!=0){  //确保版本号为HTTP/1.1
        return HC::BAD_REQUEST;
    }


    if(strncasecmp(m_url,"http://",7)==0){  //检查网址是不是http
        m_url+=7;
        m_url=strchr(m_url,'/');//必须找到第一个'/'开头的path，如http://www.baidu.com/index.html会跳过域名找到/index.html
        if(!m_url || m_url[0]!='/') return HC::BAD_REQUEST; //没有'/'返回一个BAD_REQUEST,确保m_url指向'/'

    }
    
    // else if(strncasecmp(m_url,"https://",8)==0){  //检查是不是https
    //     m_url+=8;
    //     m_url=strchr(m_url,'/');
    //     if(!m_url) return HC::BAD_REQUEST;

    // }


    m_check_state=CS::CHECK_STATE_HEADER;//解析完请求行开始解析头部字段,Http_Conn实例的状态发生转移
    return HC::NO_REQUEST;
    


}



Http_Conn::HTTP_CODE Http_Conn::parse_headers(char* text){//处理头部字段
    using CS=CHECK_STATE;
    using HC=HTTP_CODE;
    
    if(text[0]=='\0'){  //如果遇到终止符说明头部字段解析完毕
        if(m_content_length!=0){
            m_check_state=CS::CHECK_STATE_CONTENT;//有内容就进入内容处理
            return HC::NO_REQUEST;
        }

        return HC::GET_REQUEST;
    }

    else if(strncasecmp(text,"Connection:",11)==0){//Connection字段
        text+=11;
        text+=strspn(text," \t");
        if(strcasecmp(text,"keep-alive")==0){//开启长连接(默认false)
            m_linger=true;
            
        }

    }

    else if(strncasecmp(text,"Content-Length:",15)==0){//获取内容长度
        text+=15;
        text+=strspn(text," \t");
        m_content_length=atol(text);
        

    }


    else if(strncasecmp(text,"Host:",5)==0){  //获取主机号
        text+=5;
        text+=strspn(text," \t");
        m_host=text;

    }


    else{
        //printf("oop unknown header %s from sock: %d\n",text,m_sockfd);
    }


    return HC::NO_REQUEST;


}



Http_Conn::HTTP_CODE Http_Conn::parse_content(char* text){//处理消息体
    if(m_read_idx>=m_checked_idx+m_content_length){//如果数据读完整了就截断并返回GET_REQUEST,否则返回NO_NOREQUEST
        text[m_content_length]='\0';
        return HTTP_CODE::GET_REQUEST;
    }

    return HTTP_CODE::NO_REQUEST;

}


Http_Conn::HTTP_CODE Http_Conn::process_read(){//主状态机处理
    using HC=HTTP_CODE;
    using MD=METHOD;
    using CS=CHECK_STATE;
    using LS=LINE_STATUS;
    
    
    
    LS line_status=LS::LINE_OK;//初始化LS和HC
    HC ret=HC::NO_REQUEST;
    char* text=NULL;


    while(((m_check_state==CS::CHECK_STATE_CONTENT) && (line_status==LS::LINE_OK))\
            || ((line_status=parse_line())==LS::LINE_OK)){//如果正在处理消息体且行完整或者处理头部时行完整
        
        text=get_line();//获取m_start_line 一开始为0，条件里parse_line已经把m_checked_idx指向了下一个start_line
        m_start_line=m_checked_idx;//所以这里更新m_start_line
        
        // printf("i got a line %s\n",text);
        
        switch(m_check_state){
            case CS::CHECK_STATE_REQUESTLINE:{
                // printf("paring request_line\n");

                ret=parse_request_line(text);
                if(ret==HC::BAD_REQUEST){
                    return HC::BAD_REQUEST;
                }

                break;

            }

            case CS::CHECK_STATE_HEADER:{
                // printf("paring headers\n");
                
                ret=parse_headers(text);
                if(ret==HC::BAD_REQUEST) return HC::BAD_REQUEST;
                else if(ret==HC::GET_REQUEST){
                    // printf("get request\n");
                    return do_request();
                }
                

                break;

            }


            case CS::CHECK_STATE_CONTENT:{
                // printf("paring content\n");


                ret=parse_content(text);
                if(ret==HC::GET_REQUEST){
                    return do_request();
                }
                line_status=LS::LINE_OPEN;//对于content如果没读完会跳出while循环，返回NO_REQUEST
                break;
            }

            default:{

                return HC::INTERNAL_ERROR;

            }






        }

    }

    return HC::NO_REQUEST;//只有content没读完的时候会触发NO_REQUEST;


}

Http_Conn::HTTP_CODE Http_Conn::do_request(){
    using HC=HTTP_CODE;
    
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);
    strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);
    // printf("m_real_file built %s\n",m_real_file);


    if(stat(m_real_file,&m_file_stat)<0){//检查目标文件是否存在，并返回stat到m_file_stat
        printf("NO_RESOURCE\n");

        return HC::NO_RESOURCE;

    }
    
    // printf("user %d tried to read file %s \n",m_sockfd,m_real_file);

    if(!(m_file_stat.st_mode & S_IROTH)){//检查目标文件是否有被others读权限
        return HC::FORBIDDEN_REQUEST;
    }


    if(S_ISDIR(m_file_stat.st_mode)){//是否是一个文件夹，是则返回错误码
        return HC::BAD_REQUEST;
    }

    int fd=open(m_real_file,O_RDONLY);  //打开文件并设置只读

    // printf("mmap success\n");
    m_file_address=(char*)mmap(NULL,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    

    close(fd);//映射到内存中后立即关闭文件表述符，节省内核资源

    return HC::FILE_REQUEST;

    


}



void Http_Conn::unmap(){//取消映射
    if(m_file_address){
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=NULL;
    }



}



bool Http_Conn::write(){  //把iov指向的用户缓冲区的响应报文和文件内容发送出去
    int tmp=0;
    int bytes_have_sent=0;
    int bytes_to_send=m_write_idx;
    
    if(bytes_to_send==0){//如果没有数据要发
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    while(true){
        tmp=writev(m_sockfd,m_iv,m_iv_count);
        if(tmp<=-1){
            if(errno==EAGAIN){
                modfd(m_epollfd,m_sockfd,EPOLLOUT);//内核写缓冲已满，等待下一次写事件，此时其他线程也能竞争
                return true;
            }

            unmap();
            return false;

        }

        bytes_to_send-=tmp;//这里没有统计文件内容
        bytes_have_sent+=tmp;

        if(bytes_have_sent>=m_write_idx){
            unmap();
            if(m_linger){
                init();//重置读写缓冲的和主状态,m_linger初始化为false,等待下一个请求
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return true;
            }
            else{
                modfd(m_epollfd,m_sockfd,EPOLLIN);//短链接，准备关闭连接
                return false;   
            }


        }

    }




}



bool Http_Conn::add_response(const char* format,...){//添加响应，可变长参数，用来设置相应行(状态行，头部行，空白行,内容)
    if(m_write_idx>=WRITE_BUFFER_SIZE){//如果写缓冲已满
        return false;
    }

    va_list arg_list;   
    va_start(arg_list,format);//va_start初始化va_lilst，指向第一个可变长参数format,装填参数
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);
    //vsmprintf返回本该写入的len

    if(len>WRITE_BUFFER_SIZE-1-m_write_idx){
        return false;
    }

    m_write_idx+=len;//更新m_write_idx
    va_end(arg_list);//释放arg_list
    return true;


}



bool Http_Conn::add_status_line(int status ,const char* title){//给http响应添加状态码(把格式化字符串写入写缓冲)
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);//如HTTP/1.1 200 OK

}

//状态行处理完毕


bool Http_Conn::add_headers(int content_len){//添加头部字段和空白行
    add_content_length(content_len);//Content_Length
    add_linger();//Connection
    add_blank_line();//BlankLine
    return true;//为了通过编译，实际上没有人接受这个bool

}

bool Http_Conn::add_content_length(int content_len){
    return add_response("Content-Length: %d\r\n",content_len);
}

bool Http_Conn::add_linger(){
    return add_response("Connection: %s\r\n",m_linger==true ? "keepalive":"close" );
}

bool Http_Conn::add_blank_line(){
    return add_response("%s","\r\n");
}

//头部字段处理完毕




bool Http_Conn::add_content(const char* content){
        return add_response("%s",content);
}


//消息体处理完毕
//http响应处理完毕，写缓冲待发送



bool Http_Conn::process_write(HTTP_CODE ret){//仅仅是装载数据(iovec和m_write_buf)，还没有发送
    using HC=HTTP_CODE;
    
    switch(ret){
        case HC::INTERNAL_ERROR:{
            add_status_line(500,error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)){
                return false;

            }//防止content内容过长超过缓冲区限制
            break;
        
        }
        
        case HC::BAD_REQUEST:{
            add_status_line(400,error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)){
                return false;

            }
            break;
        }

        case HC::NO_RESOURCE:{
            add_status_line(404,error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)){
                return false;

            }
            break;
        }


        case HC::FORBIDDEN_REQUEST:{
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)){
                return false;

            }
            break;
        }

        case HC::FILE_REQUEST:{
            add_status_line(200,ok_200_title);
            add_headers(m_file_stat.st_size);
            if(m_file_stat.st_size!=0){//请求文件不为空
                m_iv[0].iov_base=m_write_buf;//把写缓冲的内容标记为iov1
                m_iv[0].iov_len=m_write_idx;

                m_iv[1].iov_base=m_file_address;//文件内容标记为iov2，由于文件是mmap映射的，
                m_iv[1].iov_len=m_file_stat.st_size;//文件在第一次访问时，因为触发缺页异常被调入内核pagecache，同时修改页表建立映射
                                                    //此时writev发送文件给socket，就是一个零拷贝的操作
                
                m_iv_count=2;
                return true;

            }
            else{  //文件为空
                const char* ok_string="<html><body></body></html>";//表示空网页
                add_headers(strlen(ok_string));
                if(!add_content(ok_string)){
                    return false;

                }
                
            }

            break;
        }

        default:{
            return false;

        }




    }

    m_iv[0].iov_base=m_write_buf;//除非是请求文件不为空否则都有只有用户缓冲区的内容，没有文件内容
    m_iv[0].iov_len=m_write_idx;
    m_iv_count=1;
    return true;

}


void Http_Conn::process(){//用户逻辑函数，由线程调用，必须实现
    using HC=HTTP_CODE;
    
    // printf("thread processing user %d\n",m_sockfd);

    HC read_ret=process_read();//主从状态机启动，循环读取行，并返回处理结果HC

    // printf("FILE_REQUEST\n");

    if(read_ret==HC::NO_REQUEST){
        modfd(m_epollfd,m_sockfd,EPOLLIN);//修改文件描述符事件为读，等待完整请求到达，重启oneshot，此时可以被其他线程操作
        return;

    }

    // printf("start to load http echo\n");
    bool write_ret=process_write(read_ret);//根据read_ret装载HTTP响应

    if(!write_ret){//发生错误，装载失败

        printf("load error\n");
        close_conn();//移除socket上的事件并关闭socket,同时m_sockfd置为-1,用户数量减少
        return;
    }

    // printf("ready to sent\n");
    modfd(m_epollfd,m_sockfd,EPOLLOUT);//完成装载等待发送和注册写事件 
  
    //现已改为直接发送,不再等待主线程写
           
  




}









