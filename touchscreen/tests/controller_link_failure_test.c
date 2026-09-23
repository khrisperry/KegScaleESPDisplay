#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "controller_link.h"

enum fault { NONE, EXPORT, EXPORT_SHORT, AGREE, AGREE_SHORT, IMPORT, ENCRYPT, DECRYPT };
static enum fault fault;
static unsigned destroyed;
static psa_key_id_t last_destroyed;

psa_status_t __real_psa_destroy_key(psa_key_id_t);
psa_status_t __wrap_psa_destroy_key(psa_key_id_t key) {
  destroyed++; last_destroyed = key;
  return __real_psa_destroy_key(key);
}
psa_status_t __real_psa_export_public_key(psa_key_id_t, uint8_t *, size_t, size_t *);
psa_status_t __wrap_psa_export_public_key(psa_key_id_t key, uint8_t *out, size_t cap, size_t *n) {
  if (fault == EXPORT) return PSA_ERROR_GENERIC_ERROR;
  psa_status_t r = __real_psa_export_public_key(key, out, cap, n);
  if (fault == EXPORT_SHORT) *n = 1;
  return r;
}
psa_status_t __real_psa_raw_key_agreement(psa_algorithm_t, psa_key_id_t, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
psa_status_t __wrap_psa_raw_key_agreement(psa_algorithm_t alg, psa_key_id_t key, const uint8_t *peer, size_t len, uint8_t *out, size_t cap, size_t *n) {
  if (fault == AGREE) return PSA_ERROR_GENERIC_ERROR;
  psa_status_t r = __real_psa_raw_key_agreement(alg, key, peer, len, out, cap, n);
  if (fault == AGREE_SHORT) *n = 1;
  return r;
}
psa_status_t __real_psa_import_key(const psa_key_attributes_t *, const uint8_t *, size_t, psa_key_id_t *);
psa_status_t __wrap_psa_import_key(const psa_key_attributes_t *a, const uint8_t *data, size_t len, psa_key_id_t *key) {
  if (fault == IMPORT) return PSA_ERROR_GENERIC_ERROR;
  return __real_psa_import_key(a, data, len, key);
}
psa_status_t __real_psa_aead_encrypt(psa_key_id_t, psa_algorithm_t, const uint8_t *, size_t, const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
psa_status_t __wrap_psa_aead_encrypt(psa_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce, size_t nl, const uint8_t *aad, size_t al, const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *n) {
  if (fault == ENCRYPT) { *n = 19; return PSA_ERROR_GENERIC_ERROR; }
  return __real_psa_aead_encrypt(key, alg, nonce, nl, aad, al, in, len, out, cap, n);
}
psa_status_t __real_psa_aead_decrypt(psa_key_id_t, psa_algorithm_t, const uint8_t *, size_t, const uint8_t *, size_t, const uint8_t *, size_t, uint8_t *, size_t, size_t *);
psa_status_t __wrap_psa_aead_decrypt(psa_key_id_t key, psa_algorithm_t alg, const uint8_t *nonce, size_t nl, const uint8_t *aad, size_t al, const uint8_t *in, size_t len, uint8_t *out, size_t cap, size_t *n) {
  if (fault == DECRYPT) { *n = 7; return PSA_ERROR_GENERIC_ERROR; }
  return __real_psa_aead_decrypt(key, alg, nonce, nl, aad, al, in, len, out, cap, n);
}
static void assert_key_gone(psa_key_id_t key) {
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  assert(psa_get_key_attributes(key, &a) == PSA_ERROR_INVALID_HANDLE);
  psa_reset_key_attributes(&a);
}
int main(void) {
  assert(psa_crypto_init() == PSA_SUCCESS);
  char pub[131], peer_pub[131], code[13];
  cl_session_t s = {0}, peer = {0};
  for (enum fault f = EXPORT; f <= EXPORT_SHORT; ++f) {
    fault = f; unsigned before = destroyed;
    assert(cl_keypair(&s, pub) == ESP_FAIL);
    assert(s.ephemeral == 0 && destroyed == before + 1);
    assert_key_gone(last_destroyed);
  }
  fault = NONE; assert(cl_keypair(&peer, peer_pub) == ESP_OK);
  for (enum fault f = AGREE; f <= AGREE_SHORT; ++f) {
    fault = NONE; assert(cl_keypair(&s, pub) == ESP_OK);
    psa_key_id_t key = s.ephemeral; unsigned before = destroyed;
    fault = f;
    assert(cl_agree(&s, peer_pub, pub, peer_pub, code) == ESP_FAIL);
    assert(s.ephemeral == 0 && destroyed == before + 1);
    assert_key_gone(key);
  }
  fault = NONE; assert(cl_keypair(&s, pub) == ESP_OK);
  psa_key_id_t key = s.ephemeral;
  assert(cl_agree(&s, "invalid", pub, peer_pub, code) == ESP_ERR_INVALID_ARG);
  assert(s.ephemeral == 0); assert_key_gone(key);
  cl_clear(&s); cl_clear(&peer);

  s.server = true; peer.server = false;
  uint8_t frame[CL_MAX_FRAME]; char plain[CL_MAX_PLAIN]; size_t len;
  for (enum fault f = IMPORT; f <= ENCRYPT; ++f) {
    fault = f; len = 999; unsigned before = destroyed;
    assert(cl_seal(&s, "hello", frame, &len) == ESP_FAIL);
    assert(len == 0 && s.tx_seq == 0);
    assert(destroyed == before + (f == ENCRYPT ? 1U : 0U));
    if (f == ENCRYPT) assert_key_gone(last_destroyed);
  }
  fault = NONE; assert(cl_seal(&s, "hello", frame, &len) == ESP_OK);
  assert(s.tx_seq == 1);
  for (enum fault f = IMPORT; f <= DECRYPT; ++f) {
    if (f == ENCRYPT) continue;
    fault = f; unsigned before = destroyed;
    assert(cl_open(&peer, frame, len, plain) == ESP_FAIL);
    assert(peer.rx_seq == 0);
    assert(destroyed == before + (f == DECRYPT ? 1U : 0U));
    if (f == DECRYPT) assert_key_gone(last_destroyed);
  }
  fault = NONE; assert(cl_open(&peer, frame, len, plain) == ESP_OK);
  assert(peer.rx_seq == 1 && !strcmp(plain, "hello"));
  cl_clear(&s); cl_clear(&peer);
  puts("PASS: injected export/agreement/import/AEAD failures, key cleanup, sequence preservation, retry");
}
