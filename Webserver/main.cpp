#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>


#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536 //最大的文件描述符
#define MAX_EVENT_NUMBER 10000 //监听的最大事件数量

//添加信号捕捉

void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction ac;
    memset(&ac, '\0', sizeof(ac));
    ac.sa_handler = handler;
    if(restart)
    {
        ac.sa_flags |= SA_RESTART;//中断系统调用时，返回的时候重新执行该系统调用
    }
    sigfillset(&ac.sa_mask);//sa_mask中的信号只有在该ac指向的信号处理函数执行的时候才会屏蔽的信号集合!
    assert(sigaction(sig, &ac, NULL) != -1);
}

//添加文件描述符到epoll中
extern void addfd(int epollfd,int fd,bool one_shot);
//从epoll中删除文件描述符
extern void removefd(int epollfd,int fd);
//修改文件描述符
extern void modfd(int epollfd,int fd,int ev);

int main(int argc,char* argv[]){
    if(argc<=1){
        printf("按照如下格式运行：%s port_number\n",basename(argv[0]));
        exit(-1);
    }

    //获取端口号
    int port = atoi(argv[1]);

    //对SIGPIE信号进行处理
    addsig(SIGPIPE,SIG_IGN);

    //创建并初始化线程池
    threadpool<http_conn> * pool=NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch(...)
    {
       return 1;
    }

    //创建数组用于保存所有客户端信息
    http_conn * users = new http_conn[MAX_FD];

    int listenfd = socket(PF_INET,SOCK_STREAM,0);

    //设置端口复用
    int reuse=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd,(struct sockaddr*)&address,sizeof(address));

    //监听
    listen(listenfd,5);

    //创建epoll对象，事件数组,添加
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    
    //将监听的文件描述符添加到epoll对象中
    addfd(epollfd,listenfd,false);
    http_conn::m_epollfd = epollfd;

    while(true){
       int num = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
       if((num<0)&&(errno != EINTR)){
            printf("epoll failure\nerrno = %d\n",errno);
            perror("Epoll error ");
            break;
       }

       //循环遍历事件数组
       for(int i = 0;i<num;i++){
        int sockfd=events[i].data.fd;
        if(sockfd==listenfd){
        //有客户端链接进来
        struct  sockaddr_in client_address;
        socklen_t client_addrlen=sizeof(client_address);
        
        int connfd = accept(listenfd,(struct sockaddr*)&client_address,&client_addrlen);
        
        if(http_conn::m_user_count >= MAX_FD){
            //目前达到最大连接数
            //给客户端写一个信息：服务器内部正忙
            close(connfd);
            continue;
        }

        //将新的客户的数据初始化，放到数组中
        users[connfd].init(connfd,client_address);

        }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
            //对方异常断开或者错误
            users[sockfd].close_conn();
        }else if(events[i].events & EPOLLIN){
            if(users[sockfd].read()){
                //一次性把所有数据都读完
                pool->append(users + sockfd);
            }else{
                users[sockfd].close_conn();
            }

        }else if(events[i].events & EPOLLOUT){
            if(!users[sockfd].write())//一次性写完所有数据
            {
                users[sockfd].close_conn();
            }
        }
        close(epollfd);
        close(listenfd);
        delete [] users;
        delete pool;
       }

    }

    
    return 0;
}