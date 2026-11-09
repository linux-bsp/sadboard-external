#ifndef SADBOARD_PEER_H
#define SADBOARD_PEER_H
#include <stdint.h>
#define SUITE_PEER_ADDRESS "198.19.1.2"
#define SUITE_RPS_ADDRESS "198.19.0.2"
#define SUITE_DATA_PORT 5001
#define SUITE_CONTROL_PORT 5002
struct suite_ack {
    uint32_t kind, total, checksum, cpu;
};
struct suite_datagram {
    uint32_t kind, offset, length;
    unsigned char data[1500];
};
#endif
