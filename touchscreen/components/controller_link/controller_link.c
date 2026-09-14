#include "controller_link.h"
#include <string.h>

void cl_hex(const uint8_t *in, size_t len, char *out) {
  static const char h[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = h[in[i] >> 4];
    out[i * 2 + 1] = h[in[i] & 15];
  }
  out[len * 2] = 0;
}
static int nibble(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
bool cl_unhex(const char *in, uint8_t *out, size_t len) {
  if (!in || strlen(in) != len * 2)
    return false;
  for (size_t i = 0; i < len; i++) {
    int a = nibble(in[2 * i]), b = nibble(in[2 * i + 1]);
    if (a < 0 || b < 0)
      return false;
    out[i] = (a << 4) | b;
  }
  return true;
}
static esp_err_t hash(const uint8_t *p, size_t n, uint8_t out[32]) {
  size_t used;
  return psa_hash_compute(PSA_ALG_SHA_256, p, n, out, 32, &used) == PSA_SUCCESS
             ? ESP_OK
             : ESP_FAIL;
}
esp_err_t cl_keypair(cl_session_t *s, char pub[131]) {
  if (psa_crypto_init() != PSA_SUCCESS)
    return ESP_FAIL;
  if (s->ephemeral)
    psa_destroy_key(s->ephemeral);
  s->ephemeral = 0;
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&a, 256);
  psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&a, PSA_ALG_ECDH);
  psa_status_t r = psa_generate_key(&a, &s->ephemeral);
  psa_reset_key_attributes(&a);
  if (r != PSA_SUCCESS)
    return ESP_FAIL;
  uint8_t bytes[65];
  size_t n = 0;
  if (psa_export_public_key(s->ephemeral, bytes, sizeof(bytes), &n) !=
          PSA_SUCCESS ||
      n != 65)
    return ESP_FAIL;
  cl_hex(bytes, n, pub);
  return ESP_OK;
}
esp_err_t cl_agree(cl_session_t *s, const char *peer, const char *server,
                   const char *client, char code[13]) {
  uint8_t input[32 + 65 + 65], p[65], digest[32];
  size_t n = 0;
  if (!cl_unhex(peer, p, 65) || !cl_unhex(server, input + 32, 65) ||
      !cl_unhex(client, input + 97, 65))
    return ESP_ERR_INVALID_ARG;
  psa_status_t r =
      psa_raw_key_agreement(PSA_ALG_ECDH, s->ephemeral, p, 65, input, 32, &n);
  psa_destroy_key(s->ephemeral);
  s->ephemeral = 0;
  if (r != PSA_SUCCESS || n != 32)
    return ESP_FAIL;
  esp_err_t e = hash(input, sizeof(input), s->master);
  memset(input, 0, sizeof(input));
  if (e != ESP_OK)
    return e;
  e = hash(s->master, 32, digest);
  /* Six hexadecimal characters (24 bits) are enough for this local,
   * time-limited, five-attempt user-verification step. The ECDH master key
   * remains 256 bits; only the human comparison string is shortened. */
  if (e == ESP_OK)
    cl_hex(digest, 3, code);
  return e;
}
esp_err_t cl_start(cl_session_t *s, const uint8_t challenge[32],
                   const uint8_t client_nonce[32], bool server) {
  /* A secret prefix of fixed size plus fresh 256-bit server and client
   * challenges. Keys are never reused across connections; direction is bound in
   * nonce. */
  uint8_t input[96];
  memcpy(input, s->master, 32);
  memcpy(input + 32, challenge, 32);
  memcpy(input + 64, client_nonce, 32);
  esp_err_t e = hash(input, sizeof(input), s->traffic);
  memset(input, 0, sizeof(input));
  s->server = server;
  s->tx_seq = 0;
  s->rx_seq = 0;
  return e;
}
static esp_err_t crypt(cl_session_t *s, bool encrypt, uint8_t direction,
                       uint32_t seq, const uint8_t *in, size_t n, uint8_t *out,
                       size_t capacity, size_t *used) {
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t key = 0;
  psa_set_key_type(&a, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&a, 256);
  psa_set_key_algorithm(&a, PSA_ALG_GCM);
  psa_set_key_usage_flags(&a, encrypt ? PSA_KEY_USAGE_ENCRYPT
                                      : PSA_KEY_USAGE_DECRYPT);
  psa_status_t r = psa_import_key(&a, s->traffic, 32, &key);
  psa_reset_key_attributes(&a);
  if (r != PSA_SUCCESS)
    return ESP_FAIL;
  uint8_t nonce[12] = {1, direction, 0,         0,         0,        0,
                       0, 0,         seq >> 24, seq >> 16, seq >> 8, seq};
  const uint8_t aad[] = "keg-controller-v1";
  if (encrypt)
    r = psa_aead_encrypt(key, PSA_ALG_GCM, nonce, 12, aad, sizeof(aad) - 1, in,
                         n, out, capacity, used);
  else
    r = psa_aead_decrypt(key, PSA_ALG_GCM, nonce, 12, aad, sizeof(aad) - 1, in,
                         n, out, capacity, used);
  psa_destroy_key(key);
  return r == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}
esp_err_t cl_seal(cl_session_t *s, const char *plain, uint8_t *out,
                  size_t *len) {
  size_t n = strlen(plain);
  if (n >= CL_MAX_PLAIN || s->tx_seq == UINT32_MAX)
    return ESP_ERR_INVALID_SIZE;
  uint32_t seq = ++s->tx_seq;
  out[0] = seq >> 24;
  out[1] = seq >> 16;
  out[2] = seq >> 8;
  out[3] = seq;
  esp_err_t e = crypt(s, true, s->server ? 1 : 2, seq, (const uint8_t *)plain,
                      n, out + 4, CL_MAX_FRAME - 4, len);
  *len += 4;
  return e;
}
esp_err_t cl_open(cl_session_t *s, const uint8_t *in, size_t len, char *out) {
  if (len < 20 || len > CL_MAX_FRAME)
    return ESP_ERR_INVALID_SIZE;
  uint32_t seq = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                 ((uint32_t)in[2] << 8) | in[3];
  if (!seq || seq <= s->rx_seq)
    return ESP_ERR_INVALID_STATE;
  size_t n = 0;
  esp_err_t e = crypt(s, false, s->server ? 2 : 1, seq, in + 4, len - 4,
                      (uint8_t *)out, CL_MAX_PLAIN - 1, &n);
  if (e == ESP_OK) {
    out[n] = 0;
    s->rx_seq = seq;
  }
  return e;
}
void cl_clear(cl_session_t *s) {
  if (s->ephemeral)
    psa_destroy_key(s->ephemeral);
  memset(s, 0, sizeof(*s));
}
