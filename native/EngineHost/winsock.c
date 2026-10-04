/* IPv4 Winsock 1.1/2 synchronous API bridge for the 32-bit guest.
 * Structures, option numbers, error codes and handles are explicitly translated.
 * Native descriptors never cross the guest boundary. Native sockets remain
 * nonblocking so close/cleanup can cancel a guest blocking call safely.
 * Reference: Microsoft's Windows Sockets API documentation (winsock2.h).
 */
#include "host.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { NET_MAX = 256, WIN_FD_MAX = 64, NET_INVALID = UINT32_MAX };
enum { W_INTR=10004, W_ACCESS=10013, W_FAULT=10014, W_INVAL=10022,
       W_MFILE=10024, W_AGAIN=10035, W_PROGRESS=10036, W_ALREADY=10037,
       W_NOTSOCK=10038, W_DEST=10039, W_MSGSIZE=10040, W_PROTOTYPE=10041,
       W_NOPROTOOPT=10042, W_PROTOCOL=10043, W_SOCKTYPE=10044, W_NOTSUP=10045,
       W_AF=10047, W_ADDRINUSE=10048, W_ADDR=10049, W_NETDOWN=10050,
       W_NETUNREACH=10051, W_NETRESET=10052, W_ABORT=10053, W_RESET=10054,
       W_NOBUFS=10055, W_ISCONN=10056, W_NOTCONN=10057, W_SHUTDOWN=10058,
       W_TIMEOUT=10060, W_REFUSED=10061, W_HOSTDOWN=10064, W_HOSTUNREACH=10065,
       W_VERSION=10092, W_NOTSTARTED=10093, W_HOSTNOTFOUND=11001,
       W_TRYAGAIN=11002, W_NORECOVERY=11003, W_NODATA=11004 };
typedef struct {
    uint32_t id;
    int fd, type, nonblocking, closing, connecting;
    unsigned refs, recv_ms, send_ms;
} NetSocket;
typedef struct NetLease { unsigned index; NetSocket value; struct NetLease *next; } NetLease;
static _Thread_local NetLease *net_leases;
static _Thread_local int net_cancel_state;
static NetSocket net_sockets[NET_MAX];
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned net_starts;
static uint32_t net_next_id = 0x64000000u;
static _Thread_local int net_error;
static _Thread_local uint32_t net_hostent, net_ntoa;

static int net_errno(int e) {
    switch (e) {
        case 0:return 0;
        case EINTR:return W_INTR; case EACCES:case EPERM:return W_ACCESS;
        case EFAULT:return W_FAULT; case EINVAL:return W_INVAL;
        case EMFILE:case ENFILE:return W_MFILE;
        case EAGAIN:return W_AGAIN;
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:return W_AGAIN;
#endif
        case EINPROGRESS:return W_AGAIN; case EALREADY:return W_ALREADY;
        case EBADF:case ENOTSOCK:return W_NOTSOCK;
        case EDESTADDRREQ:return W_DEST; case EMSGSIZE:return W_MSGSIZE;
        case EPROTOTYPE:return W_PROTOTYPE; case ENOPROTOOPT:return W_NOPROTOOPT;
        case EPROTONOSUPPORT:return W_PROTOCOL; case ESOCKTNOSUPPORT:return W_SOCKTYPE;
        case EOPNOTSUPP:return W_NOTSUP; case EAFNOSUPPORT:return W_AF;
        case EADDRINUSE:return W_ADDRINUSE; case EADDRNOTAVAIL:return W_ADDR;
        case ENETDOWN:return W_NETDOWN; case ENETUNREACH:return W_NETUNREACH;
        case ENETRESET:return W_NETRESET; case ECONNABORTED:return W_ABORT;
        case ECONNRESET:case EPIPE:return W_RESET; case ENOBUFS:case ENOMEM:return W_NOBUFS;
        case EISCONN:return W_ISCONN; case ENOTCONN:return W_NOTCONN;
        case ESHUTDOWN:return W_SHUTDOWN; case ETIMEDOUT:return W_TIMEOUT;
        case ECONNREFUSED:return W_REFUSED; case EHOSTDOWN:return W_HOSTDOWN;
        case EHOSTUNREACH:return W_HOSTUNREACH; default:return W_INVAL;
    }
}
static int net_fail(int e) { net_error=e; return -1; }
static int net_result(int result) { return result < 0 ? net_fail(net_errno(errno)) : result; }
static int net_span(uint32_t p, uint64_t size) {
    if (p < 0x10000u || (uint64_t)p + size > (UINT64_C(1)<<32)) {
        net_error=W_FAULT; return 0;
    }
    return 1;
}
static const char *net_string(uint32_t p) {
    if (!net_span(p,1)) return NULL;
    size_t n=(size_t)((UINT64_C(1)<<32)-p); if(n>1024)n=1024;
    if(!memchr(GPTR(p),0,n)) {net_error=W_FAULT;return NULL;}
    return GSTR(p);
}
static int net_started(void) {
    pthread_mutex_lock(&net_lock); int ok=net_starts!=0; pthread_mutex_unlock(&net_lock);
    if(!ok)net_error=W_NOTSTARTED; return ok;
}
static int net_acquire(uint32_t id, NetLease *lease) {
    pthread_mutex_lock(&net_lock);
    if(!net_starts) {pthread_mutex_unlock(&net_lock);return net_fail(W_NOTSTARTED);}
    for(unsigned i=0;i<NET_MAX;i++) if(net_sockets[i].id==id && id && !net_sockets[i].closing) {
        net_sockets[i].refs++; *lease=(NetLease){i,net_sockets[i],net_leases};net_leases=lease;
        pthread_mutex_unlock(&net_lock);return 0;
    }
    pthread_mutex_unlock(&net_lock);return net_fail(W_NOTSOCK);
}
static void net_release(NetLease *lease) {
    NetLease **link=&net_leases;while(*link && *link!=lease)link=&(*link)->next;
    if(*link)*link=lease->next;
    pthread_mutex_lock(&net_lock);NetSocket *s=&net_sockets[lease->index];
    if(!--s->refs && s->closing) {close(s->fd);memset(s,0,sizeof *s);}
    pthread_mutex_unlock(&net_lock);
}
/* Exports defer cancellation while holding references/locks. Only polling
 * reenables it; this cleanup releases all leases before the guest thread exits. */
static void net_cancel_leases(void *unused) {
    (void)unused;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL);
    while(net_leases)net_release(net_leases);
}
static int net_poll(struct pollfd *fds,nfds_t count,int timeout) {
    pthread_setcancelstate(net_cancel_state,NULL);
    if(net_cancel_state==PTHREAD_CANCEL_ENABLE)pthread_testcancel();
    int rc=poll(fds,count,timeout),saved=errno;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL);
    errno=saved;return rc;
}
static int net_alive(NetLease *lease) {
    pthread_mutex_lock(&net_lock);int ok=!net_sockets[lease->index].closing;
    pthread_mutex_unlock(&net_lock);return ok;
}
static uint32_t net_adopt(int fd,int type) {
    if(fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK)<0) {int e=errno;close(fd);net_fail(net_errno(e));return NET_INVALID;}
    fcntl(fd,F_SETFD,FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes);
#endif
    pthread_mutex_lock(&net_lock);
    if(net_starts && net_next_id < 0x7fffffffu) for(unsigned i=0;i<NET_MAX;i++) if(!net_sockets[i].id) {
        uint32_t id=++net_next_id;net_sockets[i]=(NetSocket){.id=id,.fd=fd,.type=type};
        pthread_mutex_unlock(&net_lock);return id;
    }
    int error=net_starts?W_MFILE:W_NOTSTARTED;
    pthread_mutex_unlock(&net_lock);close(fd);net_fail(error);return NET_INVALID;
}
static void net_close_all_locked(void) {
    for(unsigned i=0;i<NET_MAX;i++) if(net_sockets[i].id) {
        NetSocket *s=&net_sockets[i];s->closing=1;
        if(!s->refs){close(s->fd);memset(s,0,sizeof *s);}
    }
}
void host_winsock_shutdown(void) {
    pthread_mutex_lock(&net_lock);net_starts=0;net_close_all_locked();pthread_mutex_unlock(&net_lock);
}
static int64_t net_milliseconds(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static int net_wait(NetLease *lease,short events,int64_t deadline) {
    if(lease->value.nonblocking)return net_fail(W_AGAIN);
    for(;;) {
        if(!net_alive(lease))return net_fail(W_INTR);
        int timeout=50;
        if(deadline>=0){int64_t left=deadline-net_milliseconds();if(left<=0)return net_fail(W_TIMEOUT);if(left<timeout)timeout=(int)left;}
        struct pollfd p={lease->value.fd,events,0};int rc=net_poll(&p,1,timeout);
        if(rc>0)return 0;
        if(rc<0 && errno!=EINTR)return net_fail(net_errno(errno));
    }
}
static int net_address(uint32_t p,int len,struct sockaddr_in *out,int allow_unspec) {
    if(len<16)return net_fail(W_FAULT);
    if(!net_span(p,16))return -1;
    int family=G16(p);if(family!=2 && !(allow_unspec && !family))return net_fail(W_AF);
    memset(out,0,sizeof *out);out->sin_family=family?AF_INET:AF_UNSPEC;
#ifdef __APPLE__
    out->sin_len=sizeof *out;
#endif
    memcpy(&out->sin_port,GPTR(p+2),2);memcpy(&out->sin_addr,GPTR(p+4),4);return 0;
}
static int net_address_output_ok(uint32_t p,uint32_t len) {
    if(!net_span(len,4)||!net_span(p,16))return 0;
    if((int32_t)G32(len)<16){net_error=W_FAULT;return 0;}return 1;
}
static void net_address_output(uint32_t p,uint32_t len,const struct sockaddr_in *address) {
    memset(GPTR(p),0,16);S16(p,2);memcpy(GPTR(p+2),&address->sin_port,2);
    memcpy(GPTR(p+4),&address->sin_addr,4);S32(len,16);
}
SHIM(WSAStartup) {
    unsigned requested=ARG(0)&0xffff,major=requested&255,minor=requested>>8;
    if(!major)RET_STDCALL(W_VERSION,2);
    if(!net_span(ARG(1),400))RET_STDCALL(W_FAULT,2);
    unsigned version=major==1 ? (minor?0x0101:0x0001) : (major>2||minor>2?0x0202:requested);
    uint32_t p=ARG(1);memset(GPTR(p),0,400);S16(p,version);S16(p+2,0x0202);
    strcpy((char*)GPTR(p+4),"Master Chef IPv4 sockets");strcpy((char*)GPTR(p+261),"Running");
    S16(p+390,NET_MAX);S16(p+392,65467);
    pthread_mutex_lock(&net_lock);net_starts++;pthread_mutex_unlock(&net_lock);
    host_log("[network] Winsock initialized (version %u.%u)",version&255,version>>8);
    RET_STDCALL(0,2);
}
SHIM(WSACleanup) {
    pthread_mutex_lock(&net_lock);
    if(!net_starts){pthread_mutex_unlock(&net_lock);RET_STDCALL(net_fail(W_NOTSTARTED),0);}
    if(!--net_starts)net_close_all_locked();pthread_mutex_unlock(&net_lock);RET_STDCALL(0,0);
}
SHIM(WSAGetLastError){RET_STDCALL(net_error,0);}
SHIM(WSASetLastError){net_error=(int)ARG(0);RET_STDCALL(0,1);}
SHIM(socket) {
    if(!net_started())RET_STDCALL(NET_INVALID,3);
    if(ARG(0)!=2)RET_STDCALL(net_fail(W_AF),3);
    int type=(int)ARG(1),protocol=(int)ARG(2);
    if(type!=1 && type!=2)RET_STDCALL(net_fail(W_SOCKTYPE),3);
    int fd=socket(AF_INET,type==1?SOCK_STREAM:SOCK_DGRAM,protocol);
    RET_STDCALL(fd<0?(uint32_t)net_fail(net_errno(errno)):net_adopt(fd,type),3);
}
SHIM(closesocket) {
    uint32_t id=ARG(0);pthread_mutex_lock(&net_lock);
    if(!net_starts){pthread_mutex_unlock(&net_lock);RET_STDCALL(net_fail(W_NOTSTARTED),1);}
    for(unsigned i=0;i<NET_MAX;i++)if(id && net_sockets[i].id==id && !net_sockets[i].closing){
        NetSocket *s=&net_sockets[i];s->closing=1;if(!s->refs){close(s->fd);memset(s,0,sizeof *s);}
        pthread_mutex_unlock(&net_lock);RET_STDCALL(0,1);
    }
    pthread_mutex_unlock(&net_lock);RET_STDCALL(net_fail(W_NOTSOCK),1);
}
SHIM(bind) {
    NetLease s;struct sockaddr_in a;if(net_acquire(ARG(0),&s)<0)RET_STDCALL(-1,3);
    int rc=net_address(ARG(1),(int)ARG(2),&a,0);if(!rc)rc=net_result(bind(s.value.fd,(struct sockaddr*)&a,sizeof a));
    net_release(&s);RET_STDCALL(rc,3);
}
SHIM(connect) {
    NetLease s;struct sockaddr_in a;if(net_acquire(ARG(0),&s)<0)RET_STDCALL(-1,3);
    int rc=net_address(ARG(1),(int)ARG(2),&a,1);
    if(!rc) {
        rc=connect(s.value.fd,(struct sockaddr*)&a,sizeof a);
        if(rc<0 && errno==EINPROGRESS) {
            pthread_mutex_lock(&net_lock);net_sockets[s.index].connecting=1;pthread_mutex_unlock(&net_lock);
            rc=net_wait(&s,POLLOUT,-1);
            if(!rc){int e=0;socklen_t n=sizeof e;rc=net_result(getsockopt(s.value.fd,SOL_SOCKET,SO_ERROR,&e,&n));if(!rc && e)rc=net_fail(net_errno(e));}
        } else rc=net_result(rc);
    }
    if(!rc){pthread_mutex_lock(&net_lock);net_sockets[s.index].connecting=0;pthread_mutex_unlock(&net_lock);}
    net_release(&s);RET_STDCALL(rc,3);
}
SHIM(listen) {NetLease s;if(net_acquire(ARG(0),&s)<0)RET_STDCALL(-1,2);int rc=net_result(listen(s.value.fd,(int)ARG(1)));net_release(&s);RET_STDCALL(rc,2);}
SHIM(accept) {
    if(ARG(1) && !net_address_output_ok(ARG(1),ARG(2)))RET_STDCALL(-1,3);
    NetLease s;if(net_acquire(ARG(0),&s)<0)RET_STDCALL(-1,3);
    struct sockaddr_in a;socklen_t n=sizeof a;int fd;
    for(;;){fd=accept(s.value.fd,(struct sockaddr*)&a,&n);if(fd>=0)break;
        if(errno!=EAGAIN && errno!=EWOULDBLOCK){net_fail(net_errno(errno));break;}
        if(net_wait(&s,POLLIN,-1)<0)break;
    }
    uint32_t result=NET_INVALID;
    if(fd>=0){
        result=net_adopt(fd,1);
        if(result!=NET_INVALID){
            pthread_mutex_lock(&net_lock);
            for(unsigned i=0;i<NET_MAX;i++)if(net_sockets[i].id==result){
                net_sockets[i].nonblocking=s.value.nonblocking;
                net_sockets[i].recv_ms=s.value.recv_ms;net_sockets[i].send_ms=s.value.send_ms;break;
            }
            pthread_mutex_unlock(&net_lock);
            if(ARG(1))net_address_output(ARG(1),ARG(2),&a);
        }
    }
    net_release(&s);RET_STDCALL(result,3);
}
static int net_name(uint32_t id,uint32_t out,uint32_t len,int peer) {
    if(!net_address_output_ok(out,len))return -1;
    NetLease s;if(net_acquire(id,&s)<0)return -1;
    struct sockaddr_in a;socklen_t n=sizeof a;
    int rc=net_result(peer?getpeername(s.value.fd,(struct sockaddr*)&a,&n):getsockname(s.value.fd,(struct sockaddr*)&a,&n));
    if(!rc)net_address_output(out,len,&a);net_release(&s);return rc;
}
SHIM(getsockname){int rc=net_name(ARG(0),ARG(1),ARG(2),0);RET_STDCALL(rc,3);}
SHIM(getpeername){int rc=net_name(ARG(0),ARG(1),ARG(2),1);RET_STDCALL(rc,3);}
SHIM(shutdown){NetLease s;if(net_acquire(ARG(0),&s)<0)RET_STDCALL(-1,2);int rc=net_result(shutdown(s.value.fd,(int)ARG(1)));net_release(&s);RET_STDCALL(rc,2);}
SHIM(ioctlsocket) {
    if(!net_span(ARG(2),4))RET_STDCALL(-1,3);
    NetLease s;if(net_acquire(ARG(0),&s)<0)RET_STDCALL(-1,3);int rc=0;
    if(ARG(1)==0x8004667eu){pthread_mutex_lock(&net_lock);net_sockets[s.index].nonblocking=G32(ARG(2))!=0;pthread_mutex_unlock(&net_lock);}
    else if(ARG(1)==0x4004667fu){
        int available=0;
#ifdef SO_NREAD
        /* Darwin FIONREAD includes sockaddr/control bytes for UDP. SO_NREAD
         * reports readable payload (the next datagram, or queued TCP bytes). */
        socklen_t length=sizeof available;
        rc=net_result(getsockopt(s.value.fd,SOL_SOCKET,SO_NREAD,&available,&length));
#else
        rc=net_result(ioctl(s.value.fd,FIONREAD,&available));
#endif
        if(!rc)S32(ARG(2),(uint32_t)available);
    }
    else rc=net_fail(W_INVAL);
    net_release(&s);RET_STDCALL(rc,3);
}
static int net_flags(uint32_t flags,int sending) {
    if(flags&~7u)return net_fail(W_NOTSUP);
    int result=(flags&1?MSG_OOB:0)|(flags&2?MSG_PEEK:0)|(flags&4?MSG_DONTROUTE:0);
#ifdef MSG_NOSIGNAL
    if(sending)result|=MSG_NOSIGNAL;
#endif
    return result;
}
static int net_io(uint32_t id,uint32_t buffer,int length,uint32_t flags,
                  uint32_t address,uint32_t address_length,int sending,int addressed) {
    if(length<0)return net_fail(W_INVAL);
    if((length || buffer) && !net_span(buffer,(unsigned)length))return -1;
    int native_flags=net_flags(flags,sending);if(native_flags<0)return -1;
    struct sockaddr_in a;
    if(address && (sending ? net_address(address,(int)address_length,&a,0)<0 : !net_address_output_ok(address,address_length)))return -1;
    NetLease s;if(net_acquire(id,&s)<0)return -1;
    unsigned ms=sending?s.value.send_ms:s.value.recv_ms;int64_t deadline=ms?net_milliseconds()+ms:-1;
    int result=-1;
    for(;;) {
        if(!net_alive(&s)){result=net_fail(W_INTR);break;}
        ssize_t n;
        if(sending) n=sendto(s.value.fd,length?GPTR(buffer):NULL,(size_t)length,native_flags,
                             addressed && address?(struct sockaddr*)&a:NULL,addressed && address?sizeof a:0);
        else {
            struct iovec iov={length?GPTR(buffer):NULL,(size_t)length};
            struct msghdr msg={.msg_name=address?&a:NULL,.msg_namelen=address?sizeof a:0,.msg_iov=&iov,.msg_iovlen=1};
            n=recvmsg(s.value.fd,&msg,native_flags);
            if(n>=0 && address)net_address_output(address,address_length,&a);
            if(n>=0 && (msg.msg_flags&MSG_TRUNC)){result=net_fail(W_MSGSIZE);break;}
        }
        if(n>=0){result=(int)n;break;}
        int error=errno;
        if(error!=EAGAIN && error!=EWOULDBLOCK){result=net_fail(net_errno(error));break;}
        if(net_wait(&s,sending?POLLOUT:POLLIN,deadline)<0)break;
    }
    net_release(&s);return result;
}
SHIM(send){int rc=net_io(ARG(0),ARG(1),(int)ARG(2),ARG(3),0,0,1,0);RET_STDCALL(rc,4);}
SHIM(recv){int rc=net_io(ARG(0),ARG(1),(int)ARG(2),ARG(3),0,0,0,0);RET_STDCALL(rc,4);}
SHIM(sendto){int rc=net_io(ARG(0),ARG(1),(int)ARG(2),ARG(3),ARG(4),ARG(5),1,1);RET_STDCALL(rc,6);}
SHIM(recvfrom){int rc=net_io(ARG(0),ARG(1),(int)ARG(2),ARG(3),ARG(4),ARG(5),0,1);RET_STDCALL(rc,6);}

/* Windows fd_set is a count followed by 32-bit SOCKET tokens, not a bitset.
 * poll avoids native FD_SETSIZE restrictions and close/reuse races. */
SHIM(select) {
    uint32_t sets[3]={ARG(1),ARG(2),ARG(3)},timeout=ARG(4);
    struct pollfd polls[3*WIN_FD_MAX];NetLease leases[3*WIN_FD_MAX];
    unsigned owners[3*WIN_FD_MAX],counts[3]={0},n=0;int result=-1;
    int64_t deadline=-1;
    if(!net_started())RET_STDCALL(-1,5);
    if(timeout){if(!net_span(timeout,8))RET_STDCALL(-1,5);
        int32_t sec=(int32_t)G32(timeout),usec=(int32_t)G32(timeout+4);
        if(sec<0||usec<0||usec>=1000000)RET_STDCALL(net_fail(W_INVAL),5);
        deadline=net_milliseconds()+(int64_t)sec*1000+(usec+999)/1000;
    }
    for(unsigned k=0;k<3;k++)if(sets[k]){
        if(!net_span(sets[k],4))goto done;
        unsigned count=G32(sets[k]);if(count>WIN_FD_MAX){net_fail(W_INVAL);goto done;}
        if(!net_span(sets[k],4ull+4ull*count))goto done;
        for(unsigned j=0;j<count;j++) {
            if(net_acquire(G32(sets[k]+4+4*j),&leases[n])<0)goto done;
            owners[n]=k;polls[n]=(struct pollfd){leases[n].value.fd,k==0?POLLIN:k==1?POLLOUT:POLLPRI,0};
            if(k==2 && leases[n].value.connecting)polls[n].events|=POLLOUT;
            n++;
        }
    }
    if(!n){net_fail(W_INVAL);goto done;}
    for(;;){
        for(unsigned i=0;i<n;i++)if(!net_alive(&leases[i])){result=net_fail(W_INTR);goto done;}
        int wait=50;if(deadline>=0){int64_t left=deadline-net_milliseconds();if(left<wait)wait=left>0?(int)left:0;}
        int rc=net_poll(polls,n,wait);if(rc<0){if(errno==EINTR)continue;result=net_fail(net_errno(errno));goto done;}
        result=0;memset(counts,0,sizeof counts);
        for(unsigned i=0;i<n;i++){
            unsigned k=owners[i];short re=polls[i].revents;
            int ready=k==0?(re&(POLLIN|POLLHUP|POLLERR))!=0:k==1?(re&POLLOUT)!=0:(re&POLLPRI)!=0;
            if(leases[i].value.connecting && (re&(POLLOUT|POLLERR|POLLHUP))){
                struct sockaddr_in peer;socklen_t len=sizeof peer;
                int connected=getpeername(polls[i].fd,(struct sockaddr*)&peer,&len)==0;
                if(k==1)ready=connected;if(k==2)ready=!connected;
            }
            if(re&POLLNVAL){result=net_fail(W_NOTSOCK);goto done;}
            if(ready){counts[k]++;result++;}
            /* poll reports HUP/ERR even when not requested. Disable a descriptor
             * for this call if that state cannot satisfy this particular set. */
            if(!ready && (re&(POLLHUP|POLLERR)))polls[i].fd=-1;
            polls[i].revents=ready?1:0;
        }
        if(result || (deadline>=0 && net_milliseconds()>=deadline))break;
        /* A successful connect appears writable, but not exceptional. Avoid
         * spinning on POLLOUT when only exceptfds was requested. */
        for(unsigned i=0;i<n;i++)if(owners[i]==2 && (polls[i].events&POLLOUT)) {
            struct sockaddr_in peer;socklen_t len=sizeof peer;
            if(getpeername(polls[i].fd,(struct sockaddr*)&peer,&len)==0)polls[i].events=POLLPRI;
        }
    }
    memset(counts,0,sizeof counts);
    for(unsigned i=0;i<n;i++)if(polls[i].revents){unsigned k=owners[i];S32(sets[k]+4+4*counts[k]++,leases[i].value.id);}
    for(unsigned k=0;k<3;k++)if(sets[k])S32(sets[k],counts[k]);
done:
    for(unsigned i=0;i<n;i++)net_release(&leases[i]);
    RET_STDCALL(result,5);
}
SHIM(__WSAFDIsSet) {
    uint32_t p=ARG(1);if(!net_span(p,4))RET_STDCALL(0,2);
    unsigned n=G32(p);if(n>WIN_FD_MAX || !net_span(p,4ull+4ull*n))RET_STDCALL(0,2);
    for(unsigned i=0;i<n;i++)if(G32(p+4+4*i)==ARG(0))RET_STDCALL(1,2);RET_STDCALL(0,2);
}
static int net_option(int level,int option,int *native_level,int *native_option) {
    *native_level=level;
    if(level==0xffff){*native_level=SOL_SOCKET;switch(option){
        case 1:*native_option=SO_DEBUG;break;case 2:*native_option=SO_ACCEPTCONN;break;
        case 4:*native_option=SO_REUSEADDR;break;case 8:*native_option=SO_KEEPALIVE;break;
        case 16:*native_option=SO_DONTROUTE;break;case 32:*native_option=SO_BROADCAST;break;
        case 128:*native_option=SO_LINGER;break;case 256:*native_option=SO_OOBINLINE;break;
        case 0x1001:*native_option=SO_SNDBUF;break;case 0x1002:*native_option=SO_RCVBUF;break;
        case 0x1007:*native_option=SO_ERROR;break;case 0x1008:*native_option=SO_TYPE;break;
        default:return net_fail(W_NOPROTOOPT);
    }}else if(level==IPPROTO_TCP && option==1)*native_option=TCP_NODELAY;
    else if(level==IPPROTO_IP && option==4)*native_option=IP_TTL;
    else return net_fail(W_NOPROTOOPT);
    return 0;
}
static int net_sockopt(uint32_t id,int level,int option,uint32_t value,uint32_t size,int get) {
    if(get){if(!net_span(size,4))return -1;}
    uint32_t length=get?G32(size):size;if(length<4)return net_fail(W_FAULT);
    if(!net_span(value,4))return -1;
    NetLease s;if(net_acquire(id,&s)<0)return -1;int rc=0;
    if(level==0xffff && (option==0x1005 || option==0x1006)) {
        pthread_mutex_lock(&net_lock);unsigned *ms=option==0x1005?&net_sockets[s.index].send_ms:&net_sockets[s.index].recv_ms;
        if(get)S32(value,*ms);else if(G32(value)>INT_MAX)rc=net_fail(W_INVAL);else *ms=G32(value);
        pthread_mutex_unlock(&net_lock);
    }else{
        int lev,opt;rc=net_option(level,option,&lev,&opt);
        if(!rc && level==0xffff && option==128){
            struct linger l={G16(value),G16(value+2)};socklen_t len=sizeof l;
            rc=net_result(get?getsockopt(s.value.fd,lev,opt,&l,&len):setsockopt(s.value.fd,lev,opt,&l,sizeof l));
            if(!rc && get){S16(value,(uint16_t)l.l_onoff);S16(value+2,(uint16_t)l.l_linger);}
        }else if(!rc){
            int v=get?0:(int)G32(value);socklen_t len=sizeof v;
            rc=net_result(get?getsockopt(s.value.fd,lev,opt,&v,&len):setsockopt(s.value.fd,lev,opt,&v,sizeof v));
            if(!rc && get){if(level==0xffff && option==0x1007)v=net_errno(v);S32(value,(uint32_t)v);}
        }
    }
    if(!rc && get)S32(size,4);net_release(&s);return rc;
}
SHIM(setsockopt){int rc=net_sockopt(ARG(0),(int)ARG(1),(int)ARG(2),ARG(3),ARG(4),0);RET_STDCALL(rc,5);}
SHIM(getsockopt){int rc=net_sockopt(ARG(0),(int)ARG(1),(int)ARG(2),ARG(3),ARG(4),1);RET_STDCALL(rc,5);}
SHIM(htons){RET_STDCALL(htons((uint16_t)ARG(0)),1);}
SHIM(ntohs){RET_STDCALL(ntohs((uint16_t)ARG(0)),1);}
SHIM(htonl){RET_STDCALL(htonl(ARG(0)),1);}
SHIM(ntohl){RET_STDCALL(ntohl(ARG(0)),1);}
SHIM(inet_addr){const char *s=net_string(ARG(0));RET_STDCALL(s?inet_addr(s):UINT32_MAX,1);}
SHIM(inet_ntoa){struct in_addr a={ARG(0)};if(!net_ntoa)net_ntoa=guest_alloc(INET_ADDRSTRLEN);inet_ntop(AF_INET,&a,GPTR(net_ntoa),INET_ADDRSTRLEN);RET_STDCALL(net_ntoa,1);}
SHIM(gethostname){
    const char name[]="master-chef";
    if(!net_started())RET_STDCALL(-1,2);
    if((int)ARG(1)<(int)sizeof name || !net_span(ARG(0),sizeof name))RET_STDCALL(net_fail(W_FAULT),2);
    memcpy(GPTR(ARG(0)),name,sizeof name);RET_STDCALL(0,2);
}
static uint32_t net_host_result(const char *name,const struct in_addr *addresses,unsigned count) {
    if(!count){net_fail(W_NODATA);return 0;}
    if(!net_hostent)net_hostent=guest_alloc(1536);
    uint32_t p=net_hostent;memset(GPTR(p),0,1536);S32(p,p+32);S32(p+4,p+20);S16(p+8,2);S16(p+10,4);S32(p+12,p+1088);
    snprintf((char*)GPTR(p+32),1024,"%s",name);
    if(count>16)count=16;
    for(unsigned i=0;i<count;i++){S32(p+1088+4*i,p+1200+4*i);memcpy(GPTR(p+1200+4*i),addresses+i,4);}
    return p;
}
SHIM(gethostbyname){
    if(!net_started())RET_STDCALL(0,1);
    const char *name=ARG(0)?net_string(ARG(0)):"master-chef";if(!name)RET_STDCALL(0,1);
    struct in_addr addresses[16];unsigned count=0;
    if(!strcmp(name,"master-chef")){
        struct ifaddrs *list=NULL;
        if(getifaddrs(&list)==0){
            /* Advertise usable interfaces before loopback. */
            for(unsigned pass=0;pass<2;pass++)for(struct ifaddrs *a=list;a && count<16;a=a->ifa_next){
                if(!a->ifa_addr || a->ifa_addr->sa_family!=AF_INET || !(a->ifa_flags&IFF_UP))continue;
                if(!!(a->ifa_flags&IFF_LOOPBACK)!=(int)pass)continue;
                struct in_addr ip=((struct sockaddr_in*)a->ifa_addr)->sin_addr;
                unsigned i=0;while(i<count && addresses[i].s_addr!=ip.s_addr)i++;
                if(i==count)addresses[count++]=ip;
            }
            freeifaddrs(list);
        }
    }else{
        struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_DGRAM},*list=NULL;
        int e=getaddrinfo(name,NULL,&hints,&list);
        if(e){net_fail(e==EAI_AGAIN?W_TRYAGAIN:e==EAI_NONAME?W_HOSTNOTFOUND:W_NORECOVERY);RET_STDCALL(0,1);}
        for(struct addrinfo *a=list;a && count<16;a=a->ai_next)addresses[count++]=((struct sockaddr_in*)a->ai_addr)->sin_addr;
        freeaddrinfo(list);
    }
    uint32_t result=net_host_result(name,addresses,count);RET_STDCALL(result,1);
}
SHIM(gethostbyaddr){
    if(!net_started())RET_STDCALL(0,3);
    if(ARG(1)!=4 || ARG(2)!=2){net_fail(W_AF);RET_STDCALL(0,3);}
    if(!net_span(ARG(0),4))RET_STDCALL(0,3);
    struct sockaddr_in a={.sin_family=AF_INET};memcpy(&a.sin_addr,GPTR(ARG(0)),4);
#ifdef __APPLE__
    a.sin_len=sizeof a;
#endif
    char name[1024];int e=getnameinfo((struct sockaddr*)&a,sizeof a,name,sizeof name,NULL,0,NI_NAMEREQD);
    if(e){net_fail(e==EAI_AGAIN?W_TRYAGAIN:W_HOSTNOTFOUND);RET_STDCALL(0,3);}
    uint32_t result=net_host_result(name,&a.sin_addr,1);RET_STDCALL(result,3);
}

/* All table entries go through this boundary, including ordinal imports. */
#define NET_WRAP(n) static void net_call_##n(EngineCPU *cpu) { \
    int previous;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&previous); \
    net_cancel_state=previous;pthread_cleanup_push(net_cancel_leases,NULL); \
    shim_##n(cpu);pthread_cleanup_pop(0);pthread_setcancelstate(previous,NULL); }
NET_WRAP(accept) NET_WRAP(bind) NET_WRAP(closesocket) NET_WRAP(connect)
NET_WRAP(getpeername) NET_WRAP(getsockname) NET_WRAP(getsockopt) NET_WRAP(htonl)
NET_WRAP(htons) NET_WRAP(inet_addr) NET_WRAP(inet_ntoa) NET_WRAP(ioctlsocket)
NET_WRAP(listen) NET_WRAP(ntohl) NET_WRAP(ntohs) NET_WRAP(recv) NET_WRAP(recvfrom)
NET_WRAP(select) NET_WRAP(send) NET_WRAP(sendto) NET_WRAP(setsockopt)
NET_WRAP(shutdown) NET_WRAP(socket) NET_WRAP(gethostbyaddr) NET_WRAP(gethostbyname)
NET_WRAP(gethostname) NET_WRAP(WSAGetLastError) NET_WRAP(WSASetLastError)
NET_WRAP(WSAStartup) NET_WRAP(WSACleanup) NET_WRAP(__WSAFDIsSet)

#define NET_ORDINALS(n,o,o2) {"WSOCK32.dll",#n,net_call_##n},{"WSOCK32.dll","#"#o,net_call_##n}, \
                       {"WS2_32.dll",#n,net_call_##n},{"WS2_32.dll","#"#o2,net_call_##n}
#define NET_ENTRY(n,o) NET_ORDINALS(n,o,o)
const HostShimEntry host_shims_winsock[]={
    NET_ENTRY(accept,1),NET_ENTRY(bind,2),NET_ENTRY(closesocket,3),NET_ENTRY(connect,4),
    NET_ENTRY(getpeername,5),NET_ENTRY(getsockname,6),NET_ENTRY(getsockopt,7),NET_ENTRY(htonl,8),
    NET_ENTRY(htons,9),NET_ORDINALS(inet_addr,10,11),NET_ORDINALS(inet_ntoa,11,12),NET_ORDINALS(ioctlsocket,12,10),
    NET_ENTRY(listen,13),NET_ENTRY(ntohl,14),NET_ENTRY(ntohs,15),NET_ENTRY(recv,16),
    NET_ENTRY(recvfrom,17),NET_ENTRY(select,18),NET_ENTRY(send,19),NET_ENTRY(sendto,20),
    NET_ENTRY(setsockopt,21),NET_ENTRY(shutdown,22),NET_ENTRY(socket,23),
    NET_ENTRY(gethostbyaddr,51),NET_ENTRY(gethostbyname,52),NET_ENTRY(gethostname,57),
    NET_ENTRY(WSAGetLastError,111),NET_ENTRY(WSASetLastError,112),NET_ENTRY(WSAStartup,115),
    NET_ENTRY(WSACleanup,116),NET_ENTRY(__WSAFDIsSet,151),{NULL,NULL,NULL}
};
