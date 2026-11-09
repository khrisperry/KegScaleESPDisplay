#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

// ESP-IDF's dual-stack HTTP listener reports IPv4 connections as ::ffff:a.b.c.d.
// Match the local AP address, never the Host header or the client's source IP.
static inline bool tc001_setup_address_allowed(bool active, const struct sockaddr *address, socklen_t length) {
    static const unsigned char ap[4]={192,168,4,1};
    if(!active || !address || length<offsetof(struct sockaddr,sa_family)+sizeof(address->sa_family)) return false;
    if(address->sa_family==AF_INET) {
        if(length<sizeof(struct sockaddr_in)) return false;
        const struct sockaddr_in *v4=(const struct sockaddr_in *)address;
        return !memcmp(&v4->sin_addr,ap,sizeof(ap));
    }
    if(address->sa_family==AF_INET6) {
        if(length<sizeof(struct sockaddr_in6)) return false;
        const struct sockaddr_in6 *v6=(const struct sockaddr_in6 *)address;
        static const unsigned char mapped[12]={0,0,0,0,0,0,0,0,0,0,255,255};
        return !memcmp(v6->sin6_addr.s6_addr,mapped,sizeof(mapped)) &&
            !memcmp(v6->sin6_addr.s6_addr+12,ap,sizeof(ap));
    }
    return false;
}
