/* Real native peers exercise the public guest ABI, without game data. */
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include "../winsock.c"

uint8_t *engine_flat_base;
static uint32_t heap_next=0x100000;
static _Thread_local uint32_t stack=0x20000;
void host_log(const char *fmt, ...) {(void)fmt;}
uint32_t guest_alloc(uint32_t n) {uint32_t p=heap_next;heap_next+=(n+15)&~15u;return p;}
static uint32_t call(const char *name,unsigned n,...) {
    HostShim fn=NULL;
    for(const HostShimEntry *e=host_shims_winsock;e->name;e++)
        if(!strcmp(e->dll,"WS2_32.dll")&&!strcmp(e->name,name)){fn=e->fn;break;}
    assert(fn);
    EngineCPU cpu={0};cpu.gpr[4]=stack;S32(stack,0x12345678);
    va_list args;va_start(args,n);
    for(unsigned i=0;i<n;i++)S32(stack+4+4*i,va_arg(args,uint32_t));
    va_end(args);fn(&cpu);
    assert(cpu.pc==0x12345678 && cpu.gpr[4]==stack+4+4*n);
    assert(!net_leases);return cpu.gpr[0];
}
#define C(name,n,...) call(#name,n,##__VA_ARGS__)
#define BAD UINT32_MAX
enum { ADDR=0x30000, LEN=0x30100, BUF=0x31000, OPT=0x32000,
       READ=0x33000, WRITE=0x34000, EXCEPT=0x35000, TIME=0x36000 };
static void address(uint16_t port) {
    memset(GPTR(ADDR),0,16);S16(ADDR,2);S16(ADDR+2,htons(port));S32(ADDR+4,htonl(INADDR_LOOPBACK));
}
static void fdset(uint32_t p,uint32_t id) {S32(p,1);S32(p+4,id);}
static void nonblock(uint32_t id) {S32(OPT,1);assert(C(ioctlsocket,3,id,0x8004667eu,OPT)==0);}
static uint16_t bound_port(uint32_t id) {
    S32(LEN,16);assert(C(getsockname,3,id,ADDR,LEN)==0);
    assert(G16(ADDR)==2 && G32(LEN)==16);return ntohs(G16(ADDR+2));
}
static int peer(int type,uint16_t *port) {
    int fd=socket(AF_INET,type,0);assert(fd>=0);
    struct timeval tv={2,0};assert(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv));
    struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!bind(fd,(struct sockaddr*)&a,sizeof a));socklen_t n=sizeof a;
    assert(!getsockname(fd,(struct sockaddr*)&a,&n));*port=ntohs(a.sin_port);return fd;
}
static void startup(void) {
    assert(C(socket,3,2u,2u,0u)==BAD && C(WSAGetLastError,0)==W_NOTSTARTED);
    assert(C(WSAStartup,2,0u,BUF)==W_VERSION);
    assert(C(WSAStartup,2,0x202u,0u)==W_FAULT);
    assert(C(WSAStartup,2,0x202u,BUF)==0 && G16(BUF)==0x202);
    assert(call("#115",2,0x101u,BUF)==0 && G16(BUF)==0x101);
    assert(C(WSACleanup,0)==0);
    assert(C(socket,3,23u,2u,0u)==BAD && C(WSAGetLastError,0)==W_AF);
    assert(C(htons,1,2302u)==htons(2302));
    assert(C(htonl,1,0x12345678u)==htonl(0x12345678));
    for(const HostShimEntry *e=host_shims_winsock;e->name;e++){
        assert(e->fn);
        /* The three shifted ordinals differ between the two Windows DLLs. */
        if(e->name[0]=='#'){
            int ws2=!strcmp(e->dll,"WS2_32.dll");
            if(!strcmp(e->name,"#10"))assert(e->fn==(ws2?net_call_ioctlsocket:net_call_inet_addr));
            if(!strcmp(e->name,"#11"))assert(e->fn==(ws2?net_call_inet_addr:net_call_inet_ntoa));
            if(!strcmp(e->name,"#12"))assert(e->fn==(ws2?net_call_inet_ntoa:net_call_ioctlsocket));
        }
    }
    puts("PASS startup negotiation, errors, ordinal ABI and byte order");
}
static void udp(void) {
    uint16_t port;int fd=peer(SOCK_DGRAM,&port);
    uint32_t id=C(socket,3,2u,2u,17u);assert(id!=BAD);address(0);
    assert(C(bind,3,id,ADDR,16u)==0);assert(bound_port(id));nonblock(id);
    assert(C(recv,4,id,BUF,64u,0u)==BAD && C(WSAGetLastError,0)==W_AGAIN);
    address(port);memcpy(GPTR(BUF),"halo packet",11);
    assert(C(sendto,6,id,BUF,11u,0u,ADDR,16u)==11);
    char data[64];struct sockaddr_in from;socklen_t n=sizeof from;
    assert(recvfrom(fd,data,sizeof data,0,(struct sockaddr*)&from,&n)==11);
    assert(!memcmp(data,"halo packet",11));
    assert(sendto(fd,"response",8,0,(struct sockaddr*)&from,n)==8);
    fdset(READ,id);S32(TIME,1);S32(TIME+4,234567);
    assert(C(select,5,0u,READ,0u,0u,TIME)==1 && G32(READ)==1);
    assert(G32(TIME)==1 && G32(TIME+4)==234567);
    assert(C(__WSAFDIsSet,2,id,READ)==1);
    assert(C(ioctlsocket,3,id,0x4004667fu,OPT)==0 && G32(OPT)==8);
    S32(LEN,16);assert(C(recvfrom,6,id,BUF,64u,2u,ADDR,LEN)==8);
    assert(G16(ADDR+2)==htons(port) && G32(ADDR+4)==htonl(INADDR_LOOPBACK));
    assert(C(recvfrom,6,id,BUF,64u,0u,ADDR,LEN)==8 && !memcmp(GPTR(BUF),"response",8));
    assert(sendto(fd,"oversized",9,0,(struct sockaddr*)&from,n)==9);
    fdset(READ,id);assert(C(select,5,0u,READ,0u,0u,TIME)==1);
    assert(C(recv,4,id,BUF,3u,0u)==BAD && C(WSAGetLastError,0)==W_MSGSIZE);
    assert(C(recv,4,id,BUF,64u,0u)==BAD && C(WSAGetLastError,0)==W_AGAIN);
    assert(sendto(fd,"",0,0,(struct sockaddr*)&from,n)==0);
    fdset(READ,id);assert(C(select,5,0u,READ,0u,0u,TIME)==1);
    assert(C(recv,4,id,BUF,64u,0u)==0);
    fdset(READ,id);S32(TIME,0);S32(TIME+4,20000);int64_t began=net_milliseconds();
    assert(C(select,5,0u,READ,0u,0u,TIME)==0 && G32(READ)==0);
    assert(net_milliseconds()-began>=15 && G32(TIME+4)==20000);
    assert(C(recv,4,id,0xfffffff0u,64u,0u)==BAD && C(WSAGetLastError,0)==W_FAULT);
    assert(C(recv,4,id,BUF,BAD,0u)==BAD && C(WSAGetLastError,0)==W_INVAL);
    S32(READ,65);assert(C(select,5,0u,READ,0u,0u,TIME)==BAD);
    assert(C(select,5,0u,0u,0u,0u,TIME)==BAD && C(WSAGetLastError,0)==W_INVAL);
    S32(OPT,1);assert(C(setsockopt,5,id,0xffffu,32u,OPT,4u)==0);
    S32(LEN,4);S32(OPT,0);assert(C(getsockopt,5,id,0xffffu,32u,OPT,LEN)==0 && G32(OPT));
    assert(C(closesocket,1,id)==0);
    assert(C(closesocket,1,id)==BAD && C(WSAGetLastError,0)==W_NOTSOCK);
    assert(C(closesocket,1,(uint32_t)fd)==BAD && fcntl(fd,F_GETFD)>=0);close(fd);
    puts("PASS UDP round trip, nonblocking, select, peek, datagram truncation, bounds, stale handles");
}
static void tcp(void) {
    uint16_t port;int fd=peer(SOCK_STREAM,&port);assert(!listen(fd,2));
    uint32_t id=C(socket,3,2u,1u,6u);assert(id!=BAD);nonblock(id);address(port);
    uint32_t rc=C(connect,3,id,ADDR,16u);assert(rc==0 || (rc==BAD && C(WSAGetLastError,0)==W_AGAIN));
    fdset(WRITE,id);fdset(EXCEPT,id);S32(TIME,2);S32(TIME+4,0);
    assert(C(select,5,0u,0u,WRITE,EXCEPT,TIME)==1 && G32(WRITE)==1 && !G32(EXCEPT));
    int accepted=accept(fd,NULL,NULL);assert(accepted>=0);
    memcpy(GPTR(BUF),"tcp",3);assert(C(send,4,id,BUF,3u,0u)==3);
    char data[3];assert(recv(accepted,data,3,MSG_WAITALL)==3 && !memcmp(data,"tcp",3));
    assert(send(accepted,"reply",5,0)==5);shutdown(accepted,SHUT_WR);
    fdset(READ,id);assert(C(select,5,0u,READ,0u,0u,TIME)==1);
    assert(C(recv,4,id,BUF,64u,0u)==5);
    fdset(READ,id);assert(C(select,5,0u,READ,0u,0u,TIME)==1);assert(C(recv,4,id,BUF,64u,0u)==0);
    assert(C(closesocket,1,id)==0);close(accepted);close(fd);
    /* Use a just-released local port to exercise connection refusal. */
    fd=peer(SOCK_STREAM,&port);close(fd);id=C(socket,3,2u,1u,0u);nonblock(id);address(port);
    assert(C(connect,3,id,ADDR,16u)==BAD && C(WSAGetLastError,0)==W_AGAIN);
    fdset(WRITE,id);fdset(EXCEPT,id);
    assert(C(select,5,0u,0u,WRITE,EXCEPT,TIME)==1 && !G32(WRITE) && G32(EXCEPT)==1);
    S32(LEN,4);assert(C(getsockopt,5,id,0xffffu,0x1007u,OPT,LEN)==0 && G32(OPT)==W_REFUSED);
    C(closesocket,1,id);
    uint32_t server=C(socket,3,2u,1u,0u);address(0);assert(C(bind,3,server,ADDR,16u)==0);
    port=bound_port(server);assert(C(listen,2,server,2u)==0);nonblock(server);
    fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(!connect(fd,(struct sockaddr*)&a,sizeof a));
    fdset(READ,server);assert(C(select,5,0u,READ,0u,0u,TIME)==1);
    S32(LEN,16);id=C(accept,3,server,ADDR,LEN);assert(id!=BAD);
    assert(C(recv,4,id,BUF,1u,0u)==BAD && C(WSAGetLastError,0)==W_AGAIN);
    assert(C(shutdown,2,id,2u)==0);C(closesocket,1,id);C(closesocket,1,server);close(fd);
    puts("PASS TCP connect, accept, I/O, orderly EOF and refused-connect exception/error translation");
}
static void dns(void) {
    assert(C(gethostname,2,BUF,2u)==BAD && C(WSAGetLastError,0)==W_FAULT);
    assert(C(gethostname,2,BUF,64u)==0 && !strcmp(GSTR(BUF),"master-chef"));
    assert(C(gethostbyname,1,BUF));
    strcpy(GPTR(BUF),"localhost");uint32_t h=C(gethostbyname,1,BUF);assert(h);
    assert(G16(h+8)==2 && G16(h+10)==4 && G32(G32(h+4))==0);
    uint32_t list=G32(h+12);assert(G32(list) && G32(G32(list))==htonl(INADDR_LOOPBACK));
    strcpy(GPTR(BUF),"127.0.0.1");uint32_t ip=C(inet_addr,1,BUF);
    assert(ip==htonl(INADDR_LOOPBACK));assert(call("#11",1,BUF)==ip);
    assert(!strcmp(GSTR(call("#12",1,ip)),"127.0.0.1"));assert(!strcmp(GSTR(C(inet_ntoa,1,ip)),"127.0.0.1"));
    puts("PASS DNS guest hostent layout and IPv4 string conversion");
}
typedef struct {uint32_t id;int mode;atomic_int started;uint32_t result,error;} Waiter;
static void *wait_thread(void *opaque) {
    Waiter *w=opaque;stack=0x40000;
    C(WSASetLastError,1,4321u);assert(C(WSAGetLastError,0)==4321);w->started=1;
    if(w->mode){S32(0x42000,1);S32(0x42004,w->id);w->result=C(select,5,0u,0x42000u,0u,0u,0u);}
    else w->result=C(recv,4,w->id,0x41000u,64u,0u);
    w->error=C(WSAGetLastError,0);return NULL;
}
static void cancellation(void) {
    for(int mode=0;mode<4;mode++){
        uint32_t id=C(socket,3,2u,2u,0u);address(0);assert(C(bind,3,id,ADDR,16u)==0);
        C(WSASetLastError,1,1234u);Waiter w={.id=id,.mode=mode==1};pthread_t t;
        assert(!pthread_create(&t,NULL,wait_thread,&w));
        while(!w.started)usleep(1000);usleep(80000);
        assert(C(WSAGetLastError,0)==1234);
        if(mode==2){assert(!pthread_cancel(t));}
        else if(mode==3){assert(C(WSACleanup,0)==0);}
        else assert(C(closesocket,1,id)==0);
        void *out;assert(!pthread_join(t,&out));
        if(mode==2){assert(out==PTHREAD_CANCELED);assert(C(closesocket,1,id)==0);}
        else assert(w.result==BAD && w.error==W_INTR);
        for(unsigned i=0;i<NET_MAX;i++)assert(!net_sockets[i].id);
        if(mode==3)assert(C(WSAStartup,2,0x202u,BUF)==0);
    }
    uint32_t id=C(socket,3,2u,2u,0u);address(0);assert(C(bind,3,id,ADDR,16u)==0);
    S32(OPT,25);assert(C(setsockopt,5,id,0xffffu,0x1006u,OPT,4u)==0);
    assert(C(recv,4,id,BUF,64u,0u)==BAD && C(WSAGetLastError,0)==W_TIMEOUT);
    host_winsock_shutdown();assert(C(WSACleanup,0)==BAD && C(WSAGetLastError,0)==W_NOTSTARTED);
    puts("PASS thread-local errors, blocking timeout, close/cleanup cancellation and thread termination");
}
int main(void) {
    engine_flat_base=mmap(NULL,UINT64_C(1)<<32,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(engine_flat_base!=MAP_FAILED);assert(!mprotect(engine_flat_base,0x10000,PROT_NONE));
    startup();udp();tcp();dns();cancellation();
    assert(!munmap(engine_flat_base,UINT64_C(1)<<32));
    puts("PASS real socket bridge ABI; game connection and cross-network play require separate validation");
}
