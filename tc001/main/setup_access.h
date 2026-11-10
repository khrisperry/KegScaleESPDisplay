#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

// ESP-IDF's dual-stack HTTP listener reports IPv4 connections as ::ffff:a.b.c.d.
// Match the local AP address, never the Host header or the client's source IP.
static inline bool tc001_address_matches_ipv4(const struct sockaddr *address, socklen_t length, const unsigned char ip[4]) {
    static const unsigned char zero[4]={0};
    if(!ip || !memcmp(ip,zero,4) || !address || length<offsetof(struct sockaddr,sa_family)+sizeof(address->sa_family)) return false;
    if(address->sa_family==AF_INET) {
        if(length<sizeof(struct sockaddr_in)) return false;
        const struct sockaddr_in *v4=(const struct sockaddr_in *)address;
        return !memcmp(&v4->sin_addr,ip,4);
    }
    if(address->sa_family==AF_INET6) {
        if(length<sizeof(struct sockaddr_in6)) return false;
        const struct sockaddr_in6 *v6=(const struct sockaddr_in6 *)address;
        static const unsigned char mapped[12]={0,0,0,0,0,0,0,0,0,0,255,255};
        return !memcmp(v6->sin6_addr.s6_addr,mapped,sizeof(mapped)) &&
            !memcmp(v6->sin6_addr.s6_addr+12,ip,4);
    }
    return false;
}

static inline bool tc001_address_matches_ipv6(const struct sockaddr *address,socklen_t length,const unsigned char ip[16]) {
    static const unsigned char zero[16]={0};
    if(!address || !ip || !memcmp(ip,zero,16) || length<sizeof(struct sockaddr_in6) || address->sa_family!=AF_INET6) return false;
    const struct sockaddr_in6 *v6=(const struct sockaddr_in6 *)address;
    return !memcmp(v6->sin6_addr.s6_addr,ip,16);
}
static inline bool tc001_setup_address_allowed(bool active,const struct sockaddr *address,socklen_t length) {
    static const unsigned char ap[4]={192,168,4,1};
    return active && tc001_address_matches_ipv4(address,length,ap);
}
