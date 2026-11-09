#include <assert.h>
#include <stdio.h>
#include <arpa/inet.h>
#include "setup_access.h"
int main(void) {
    struct sockaddr_in v4={.sin_family=AF_INET};
    assert(inet_pton(AF_INET,"192.168.4.1",&v4.sin_addr)==1);
    assert(tc001_setup_address_allowed(true,(struct sockaddr *)&v4,sizeof(v4)));
    assert(!tc001_setup_address_allowed(false,(struct sockaddr *)&v4,sizeof(v4)));
    assert(!tc001_setup_address_allowed(true,(struct sockaddr *)&v4,sizeof(v4)-1));
    assert(inet_pton(AF_INET,"192.168.90.10",&v4.sin_addr)==1);
    assert(!tc001_setup_address_allowed(true,(struct sockaddr *)&v4,sizeof(v4)));
    struct sockaddr_in6 v6={.sin6_family=AF_INET6};
    assert(inet_pton(AF_INET6,"::ffff:192.168.4.1",&v6.sin6_addr)==1);
    assert(tc001_setup_address_allowed(true,(struct sockaddr *)&v6,sizeof(v6)));
    assert(!tc001_setup_address_allowed(false,(struct sockaddr *)&v6,sizeof(v6)));
    // Regression: the former sockaddr_in buffer truncated the mapped IPv6 address.
    assert(!tc001_setup_address_allowed(true,(struct sockaddr *)&v6,sizeof(v4)));
    assert(inet_pton(AF_INET6,"::ffff:192.168.90.10",&v6.sin6_addr)==1);
    assert(!tc001_setup_address_allowed(true,(struct sockaddr *)&v6,sizeof(v6)));
    assert(inet_pton(AF_INET6,"::192.168.4.1",&v6.sin6_addr)==1);
    assert(!tc001_setup_address_allowed(true,(struct sockaddr *)&v6,sizeof(v6)));
    assert(inet_pton(AF_INET6,"fe80::c0a8:401",&v6.sin6_addr)==1);
    assert(!tc001_setup_address_allowed(true,(struct sockaddr *)&v6,sizeof(v6)));
    assert(!tc001_setup_address_allowed(true,NULL,0));
    struct sockaddr unknown={.sa_family=AF_UNSPEC};
    assert(!tc001_setup_address_allowed(true,&unknown,sizeof(unknown)));
    puts("PASS: setup AP IPv4 and mapped IPv6 access, inactive setup, LAN/native IPv6 denial, truncated addresses");
}
