#pragma once
#include "esp_err.h"
#include "psa/crypto.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CL_MAX_PLAIN 2048
#define CL_MAX_FRAME (CL_MAX_PLAIN + 24)
#define CL_PUBLIC_SIZE 65
/* Version 1: P-256 ECDH pairing with a 24-bit user-compared hexadecimal
 * string. AES-256-GCM traffic keys are derived per connection. */
typedef struct {
  psa_key_id_t ephemeral;
  uint8_t master[32];
  uint8_t traffic[32];
  uint32_t tx_seq, rx_seq;
  bool server;
} cl_session_t;
void cl_hex(const uint8_t *in, size_t len, char *out);
bool cl_unhex(const char *in, uint8_t *out, size_t len);
esp_err_t cl_keypair(cl_session_t *s, char public_hex[131]);
esp_err_t cl_agree(cl_session_t *s, const char *peer_hex,
                   const char *server_hex, const char *client_hex,
                   char code[13]);
esp_err_t cl_start(cl_session_t *s, const uint8_t challenge[32],
                   const uint8_t client_nonce[32], bool server);
esp_err_t cl_seal(cl_session_t *s, const char *plain, uint8_t *out,
                  size_t *len);
esp_err_t cl_open(cl_session_t *s, const uint8_t *in, size_t len, char *out);
void cl_clear(cl_session_t *s);
#ifdef __cplusplus
}
#endif
