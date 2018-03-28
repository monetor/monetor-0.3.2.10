/**
 * \file mt_common.c
 *
 * General purpose module that houses basic useful functionality for various
 * users of the moneTor payment scheme. This module will be likely updated
 * frequently as the scheme is expanded
 */

#pragma GCC diagnostic ignored "-Wswitch-enum"
#pragma GCC diagnostic ignored "-Wstack-protector"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "or.h"
#include "buffers.h"
#include "config.h"
#include "compat.h"
#include "circuitlist.h"
#include "circuituse.h"
#include "circuitbuild.h"
#include "mt_crypto.h" // only needed for the defined byte array sizes
#include "mt_common.h"
#include "mt_cclient.h"
#include "mt_crelay.h"
#include "mt_cintermediary.h"
#include "mt_cledger.h"
#include "mt_tokens.h"
#include "router.h"
#include "relay.h"
#include "scheduler.h"

static uint64_t count[2] = {0, 0};

/**
 * Converts a public key into an address for use on the ledger. The address is
 * generated by a simple hash of the public key and is 20 bytes long.
 */
int mt_pk2addr(byte (*pk)[MT_SZ_PK], byte (*addr_out)[MT_SZ_ADDR]){
  byte hash[MT_SZ_HASH];
  mt_crypt_hash(*pk, MT_SZ_PK, &hash);
  memcpy(*addr_out, hash, MT_SZ_ADDR);
  return MT_SUCCESS;
}

/**
 * Convert a byte string into a digest for digestmap_t
 */
void mt_bytes2digest(byte* str, int str_size, byte (*digest_out)[DIGEST_LEN]){
  byte hash[MT_SZ_HASH];
  mt_crypt_hash(str, str_size, &hash);
  memcpy(*digest_out, hash, DIGEST_LEN);
}

/**
 * Converts an mt_desc_t into an address for use in digestmaps. The output is
 * a hash of the mt_desc_t contents truncated to 20 bytes
 */
void mt_desc2digest(mt_desc_t* desc, byte (*digest_out)[DIGEST_LEN]){
  byte hash[MT_SZ_HASH];
  byte input[sizeof(uint64_t) * 2 + sizeof(desc->party)];
  memcpy(input, &desc->id, sizeof(uint64_t) * 2);
  memcpy(input + sizeof(uint64_t) * 2, &desc->party, sizeof(desc->party));
  mt_crypt_hash(input, sizeof(uint64_t) * 2 + sizeof(desc->party), &hash);
  memcpy(*digest_out, hash, DIGEST_LEN);
}

/**
 * Convert a moneTor nan_any_public_t into a digest for digestmap_t
 */
void mt_nanpub2digest(nan_any_public_t* token, byte (*digest_out)[DIGEST_LEN]){
  byte hash[MT_SZ_HASH];
  byte input[sizeof(int) * 3 + MT_SZ_HASH];
  memcpy(input + sizeof(int) * 0, &token->val_from, sizeof(int));
  memcpy(input + sizeof(int) * 1, &token->val_to, sizeof(int));
  memcpy(input + sizeof(int) * 2, &token->num_payments, sizeof(int));
  memcpy(input + sizeof(int) * 3, &token->hash_tail, MT_SZ_HASH);
  mt_crypt_hash(input, sizeof(int) * 3 + MT_SZ_HASH, &hash);
  memcpy(*digest_out, hash, DIGEST_LEN);
}

/**
 * Converts byte string to a malloc'd hex output (c-string)
 */
int mt_bytes2hex(byte* bytes, int size, char** hex_out){

  *hex_out = malloc(size * 2 + 3);

  (*hex_out)[0] = '0';
  (*hex_out)[1] = 'x';

  for(int i = 0; i < size; i++)
    sprintf(*hex_out + i * 2 + 2, "%02X", bytes[i]);

  (*hex_out)[size * 2 + 2] = '\0';
  return MT_SUCCESS;
}

/**
 * Converts a hex digest (c-string) into a malloc'd byte string; return size
 */
int mt_hex2bytes(const char* hex, byte** bytes_out){

  tor_assert(strlen(hex) >= 2);
  int size = (strlen(hex) - 2) / 2;
  *bytes_out = malloc(size);

  for(int i = 0; i < size; i++)
    sscanf(hex + i * 2 + 2, "%2hhx", &(*bytes_out)[i]);

  return size;
}

/**
 * Compute a hash chain of the given size using the given random head. The
 * output is written to the inputted hc_out address, which is a pointer to a
 * arbitrary sized array of pointers to MT_SZ_HASH arrays. The ordering is such
 * that the tail of the chain is at the front of the array and the head is at
 * the rear.
 */
int mt_hc_create(int size, byte (*head)[MT_SZ_HASH], byte (*hc_out)[][MT_SZ_HASH]){
  if(size < 1)
    return MT_ERROR;

  memcpy(&((*hc_out)[size -1]), *head, MT_SZ_HASH);	\

    for(int i = size - 2; i >= 0; i--){
      if(mt_crypt_hash((*hc_out)[i+1], MT_SZ_HASH, &((*hc_out)[i])) != MT_SUCCESS)
        return MT_ERROR;
    }
  return MT_SUCCESS;
}

/**
 * Verifies the claim that a given preimage is in fact the kth element on a hash
 * chain starting at the given tail.
 */
int mt_hc_verify(byte (*tail)[MT_SZ_HASH], byte (*preimage)[MT_SZ_HASH], int k){
  byte current[MT_SZ_HASH];
  byte temp[MT_SZ_HASH];

  memcpy(current, *preimage, MT_SZ_HASH);
  for(int i = 0; i < k; i++){
    if(mt_crypt_hash(current, MT_SZ_HASH, &temp) != MT_SUCCESS)
      return MT_ERROR;
    memcpy(current, temp, MT_SZ_HASH);
  }

  if(memcmp(current, *tail, MT_SZ_HASH) != 0){
    log_warn(LD_MT, "MoneTor: hash chain component did not verify");
    return MT_ERROR;
  }

  return MT_SUCCESS;
}

/**
 * Takes two mt_desc_t structures and compares them similarly to memcmp
 */
int mt_desc_comp(mt_desc_t* desc1, mt_desc_t* desc2){
  if(desc1->party != desc2->party)
    return (desc1->party > desc2->party) ? 1 : -1;
  if(desc1->id[0] != desc2->id[0])
    return (desc1->id[0] > desc2->id[0]) ? 1 : -1;
  if(desc1->id[1] != desc2->id[1])
    return (desc1->id[1] > desc2->id[1]) ? 1 : -1;
  return 0;
}

/**
 * Return a string describing the party type for printing
 */
const char* mt_party_describe(mt_party_t party){
  switch(party){
    case MT_PARTY_CLI:
      return "MT_PARTY_CLI";
    case MT_PARTY_REL:
      return "MT_PARTY_REL";
    case MT_PARTY_INT:
      return "MT_PARTY_INT";
    case MT_PARTY_AUT:
      return "MT_PARTY_AUT";
    case MT_PARTY_LED:
      return "MT_PARTY_LED";
    case MT_PARTY_IDK:
      return "MT_PARTY_IDK";
    case MT_PARTY_END:
      return "MT_PARTY_END";
    default:
      log_warn(LD_MT, "BUG - unknown party %hhx", party);
      return "";
  }
}

/**
 * Create a signed receipt of a ledger transaction
 */
int mt_receipt_sign(any_led_receipt_t* rec, byte (*sk)[MT_SZ_SK]){

  // construct the packed string
  int str_size = sizeof(mt_ntype_t) + sizeof(int) + MT_SZ_ADDR + MT_SZ_ADDR;
  byte str[str_size];

  memcpy(str, &rec->type, sizeof(mt_ntype_t));
  memcpy(str + sizeof(mt_ntype_t), &rec->val, sizeof(int));
  memcpy(str + sizeof(mt_ntype_t) + sizeof(int), rec->from, MT_SZ_ADDR);
  memcpy(str + sizeof(mt_ntype_t) + sizeof(int) + MT_SZ_ADDR, rec->to, MT_SZ_ADDR);

  return mt_sig_sign(str, str_size, sk, &rec->sig);
}

/**
 * Verify the receipt of a ledger transaction
 */
int mt_receipt_verify(any_led_receipt_t* rec, byte (*pk)[MT_SZ_PK]){

  // construct the packed string
  int str_size = sizeof(mt_ntype_t) + sizeof(int) + MT_SZ_ADDR + MT_SZ_ADDR;
  byte str[str_size];

  memcpy(str, &rec->type, sizeof(mt_ntype_t));
  memcpy(str + sizeof(mt_ntype_t), &rec->val, sizeof(int));
  memcpy(str + sizeof(mt_ntype_t) + sizeof(int), rec->from, MT_SZ_ADDR);
  memcpy(str + sizeof(mt_ntype_t) + sizeof(int) + MT_SZ_ADDR, rec->to, MT_SZ_ADDR);

  return mt_sig_verify(str, str_size, pk, &rec->sig);
}

/**
 * Populates the unsigned fields of a new micropayment wallet using a given old
 * wallet and a desired value change
 */
int mt_wallet_create(byte (*pp)[MT_SZ_PP], int value, chn_end_wallet_t* wal_old,
		     chn_end_wallet_t* wal_new){

  int errors = 0;

  // transfer straightforward values first
  wal_new->end_bal = wal_old->end_bal + value;
  wal_new->int_bal = wal_old->int_bal - value;
  memcpy(wal_new->int_pk, wal_old->int_pk, MT_SZ_PK);
  memcpy(wal_new->csk, wal_old->csk, MT_SZ_SK);

  errors += mt_crypt_keygen(pp, &wal_new->wpk, &wal_new->wsk);
  errors += mt_crypt_rand(MT_SZ_HASH, wal_new->rand);

  // generate wallet commitment
  int com_msg_size = MT_SZ_PK + sizeof(int);
  byte com_msg[com_msg_size];
  memcpy(com_msg, wal_new->wpk, MT_SZ_PK);
  memcpy(com_msg + MT_SZ_PK, &wal_new->end_bal, sizeof(int));
  errors += mt_com_commit(com_msg, com_msg_size, &wal_new->rand, &wal_new->wcom);

  // public zkp parameters
  int public_size = MT_SZ_PK + sizeof(int) + MT_SZ_PK + MT_SZ_COM;
  byte public[public_size];
  memcpy(public, wal_old->int_pk, MT_SZ_PK);
  memcpy(public + MT_SZ_PK, &value, sizeof(int));
  memcpy(public + MT_SZ_PK + sizeof(int), wal_old->wpk, MT_SZ_PK);
  memcpy(public + MT_SZ_PK + sizeof(int) + MT_SZ_PK, wal_new->wcom, MT_SZ_COM);

  // prove knowledge of the following values
  int hidden_size = MT_SZ_PK + sizeof(int) + MT_SZ_HASH + MT_SZ_SIG;
  byte hidden[hidden_size];
  memcpy(hidden, wal_new->wpk, MT_SZ_PK);
  memcpy(hidden + MT_SZ_PK, &wal_new->end_bal, sizeof(int));
  memcpy(hidden + MT_SZ_PK + sizeof(int), wal_new->rand, MT_SZ_HASH);
  memcpy(hidden + MT_SZ_PK + sizeof(int) + MT_SZ_HASH, wal_old->sig, MT_SZ_SIG);

  errors += mt_zkp_prove(MT_ZKP_TYPE_2, pp, public, public_size, hidden, hidden_size, &wal_new->zkp);

  if(errors != MT_SUCCESS * 4){
    log_warn(LD_MT, "MoneTor: error creating wallet");
    return MT_ERROR;
  }

  return MT_SUCCESS;

}

void increment(long unsigned *id) {
  id[0]++;
  if(id[0]==0) {
    if(++id[1]==0)
      id[0]=0;
  }
}

uint64_t rand_uint64(void) {
  srand(getpid());
  uint64_t r = 0;
  for (int i=0; i<64; i += 30) {
    r = r*((uint64_t)RAND_MAX + 1) + rand();
  }
  return r;
}

/*
 * Should be called by the tor_init() function - initialize all environment
 * for the payment system
 * XXX TODO
 */
void mt_init(void){
  log_info(LD_MT, "MoneTor: Initializing the payment system");
  count[0] = rand_uint64();
  count[1] = rand_uint64();
  /** Only one should properly complete */
  if (ledger_mode(get_options())) {
    mt_cledger_init();
  }
  else if (intermediary_mode(get_options())) {
    mt_cintermediary_init();
  }
  else if (server_mode(get_options())) {
    mt_crelay_init();
  }
  else {
    mt_cclient_init();
  }
}
/**
 * Initialize ledger info
 */
void
ledger_init(ledger_t **ledger, const node_t *node, extend_info_t *ei,
    time_t now) {
  tor_assert(node);
  tor_assert(ei);
  *ledger = tor_malloc_zero(sizeof(ledger_t));
  memcpy((*ledger)->identity.identity, node->identity, DIGEST_LEN);
  (*ledger)->is_reachable = LEDGER_REACHABLE_MAYBE;
  /*increment(count);*/
  (*ledger)->desc.id[0] = 0;
  (*ledger)->desc.id[1] = 0;
  (*ledger)->desc.party = MT_PARTY_LED;
  (*ledger)->ei = ei;
  log_info(LD_MT, "Ledger created at %lld", (long long) now);
}

void ledger_free(ledger_t **ledger) {
  if (!ledger || !*ledger)
    return;
  if ((*ledger)->ei)
    extend_info_free((*ledger)->ei);
  tor_free(*ledger);
}

/**
 * Verifies enough money remains in the wallet - NOT URGENT
 */
int mt_check_enough_fund(void) {
  return 1;
}

/**
 * Run scheduled events of the payment systems. Get called every second and
 * verifies that everything holds.
 */
void monetor_run_scheduled_events(time_t now) {

  if (ledger_mode(get_options())) {
    run_cledger_scheduled_events(now);
  }
  else if (intermediary_mode(get_options())) {
    run_cintermediary_scheduled_events(now);
  }
  else if (server_mode(get_options())) {
    run_crelay_scheduled_events(now);
  }
  else {
    /*2run scheduled cclient event - avoid to do this on authority*/
    run_cclient_scheduled_events(now);
  }
}

/**
 * Returns a description of this desc. Mostly used for log
 * purpose
 *
 * XXX MoneTor Todo
 */

const char* mt_desc_describe(mt_desc_t* desc) {
  char *partychar = NULL;
  switch (desc->party) {
    case MT_PARTY_CLI: tor_asprintf(&partychar, "%s", "MT_PARTY_CLI");
                       break;
    case MT_PARTY_REL: tor_asprintf(&partychar, "%s", "MT_PARTY_REL");
                       break;
    case MT_PARTY_INT: tor_asprintf(&partychar, "%s", "MT_PARTY_INT");
                       break;
    case MT_PARTY_LED: tor_asprintf(&partychar, "%s", "MT_PARTY_LED");
                       break;
    case MT_PARTY_END: tor_asprintf(&partychar, "%s", "MT_PARTY_END");
                       break;
    case MT_PARTY_IDK: tor_asprintf(&partychar, "%s", "MT_PARTY_IDK");
                       break;
    case MT_PARTY_AUT: tor_asprintf(&partychar, "%s", "MT_PARTY_AUT");
                       break;
    default: tor_asprintf(&partychar, "%s", "");
  }
  char *ret = NULL;
  tor_asprintf(&ret, "id 0: %lu, 1: %lu, party: %s", desc->id[0], desc->id[1], partychar);
  return ret;
}

/**
 * Returns 1 if both structure have the same digest
 * 0 otherwise
 */

int mt_desc_eq(mt_desc_t* desc1, mt_desc_t* desc2) {
  byte id1[DIGEST_LEN];
  byte id2[DIGEST_LEN];
  mt_desc2digest(desc1, &id1);
  mt_desc2digest(desc2, &id2);
  return tor_memeq(id1, id2, DIGEST_LEN);
}

const char* mt_signal_describe(mt_signal_t signal) {
  switch (signal) {
    case MT_SIGNAL_PAYMENT_SUCCESS: return "Last mt_cpay_pay call is successful";
    case MT_SIGNAL_PAYMENT_FAILURE: return "Last mt_cpay_pay call has failed";
    case MT_SIGNAL_CLOSE_SUCCESS: return "Last mt_cpay_close is successfull";
    case MT_SIGNAL_CLOSE_FAILURE: return "Last mt_cpay_close has failed";
    case MT_SIGNAL_PAYMENT_INITIALIZED: return "A client initialized a payment";
    case MT_SIGNAL_PAYMENT_RECEIVED: return "A payement has been received";
    case MT_SIGNAL_INTERMEDIARY_IDLE: return "No active nanopayment channel left with an interemediary";
    default:
      log_info(LD_MT, "Signal event description unsupported: %d", (int) signal);
      return "Event unsupported";
  }
}

/** Free mt_desc */
void mt_desc_free(mt_desc_t *desc) {
  if (!desc)
    return;
  // XXX todo
}

/**
 * Pack the relay header containing classical relay_header_t
 * and our payment header
 */

void relay_pheader_pack(uint8_t *dest, const relay_header_t* rh,
    relay_pheader_t* rph) {
  set_uint8(dest, rh->command);
  set_uint16(dest+1, htons(rh->recognized));
  set_uint16(dest+3, htons(rh->stream_id));
  memcpy(dest+5, rh->integrity, 4);
  set_uint16(dest+9, htons(rh->length));
  set_uint8(dest+11, rph->pcommand);
  set_uint16(dest+12, htons(rph->length));
}

/** Unpack the network order buffer src into relay_pheader_t
 * struct
 */
void relay_pheader_unpack(relay_pheader_t *dest, const uint8_t *src) {
  dest->pcommand = get_uint8(src);
  dest->length = ntohs(get_uint16(src+1));
}

void direct_pheader_pack(uint8_t *dest, relay_pheader_t *rph) {
  set_uint8(dest, rph->pcommand);
  set_uint16(dest+1, htons(rph->length));
}

int pack_int_id(byte **msg, int_id_t *ind_id) {
  *msg = tor_malloc_zero(sizeof(int_id_t));
  memcpy(*msg, ind_id, sizeof(int_id_t));
  return sizeof(int_id_t);
}

void unpack_int_id(byte *msg, int_id_t *int_id_out) {
  memcpy(int_id_out, msg, sizeof(int_id_t));
}

void mt_update_payment_window(circuit_t *circ) {
  if (server_mode(get_options())) {
    mt_crelay_update_payment_window(circ);
  }
  else {
    /** We must be a client */
    mt_cclient_update_payment_window(circ, 0);
  }
}

static mt_party_t
mt_common_whose_other_edge(mt_ntype_t pcommand) {
  switch (pcommand) {
    case MT_NTYPE_NAN_CLI_DESTAB1:
    case MT_NTYPE_NAN_CLI_DPAY1:
    case MT_NTYPE_MIC_CLI_PAY1:
    case MT_NTYPE_MIC_CLI_PAY3:
    case MT_NTYPE_MIC_CLI_PAY5:
    case MT_NTYPE_NAN_CLI_SETUP1:
    case MT_NTYPE_NAN_CLI_SETUP3:
    case MT_NTYPE_NAN_CLI_SETUP5:
    case MT_NTYPE_NAN_CLI_ESTAB1:
    case MT_NTYPE_NAN_CLI_PAY1:
    case MT_NTYPE_NAN_CLI_REQCLOSE1:
      return  MT_PARTY_CLI;
    case MT_NTYPE_MIC_REL_PAY2:
    case MT_NTYPE_MIC_REL_PAY6:
    case MT_NTYPE_NAN_REL_ESTAB2:
    case MT_NTYPE_NAN_REL_ESTAB4:
    case MT_NTYPE_NAN_REL_ESTAB6:
    case MT_NTYPE_NAN_REL_PAY2:
    case MT_NTYPE_NAN_REL_REQCLOSE2:
      return MT_PARTY_REL;
    case MT_NTYPE_CHN_END_ESTAB1:
    case MT_NTYPE_CHN_END_ESTAB3:
    case MT_NTYPE_NAN_END_CLOSE1:
    case MT_NTYPE_NAN_END_CLOSE3:
    case MT_NTYPE_NAN_END_CLOSE5:
    case MT_NTYPE_NAN_END_CLOSE7:
    case MT_NTYPE_CHN_END_SETUP:
    case MT_NTYPE_CHN_END_CLOSE:
    case MT_NTYPE_CHN_END_CASHOUT:
      return MT_PARTY_END;
    case MT_NTYPE_MAC_AUT_MINT:
      return MT_PARTY_AUT;
    case MT_NTYPE_CHN_INT_ESTAB2:
    case MT_NTYPE_CHN_INT_ESTAB4:
    case MT_NTYPE_MIC_INT_PAY4:
    case MT_NTYPE_MIC_INT_PAY7:
    case MT_NTYPE_MIC_INT_PAY8:
    case MT_NTYPE_NAN_INT_SETUP2:
    case MT_NTYPE_NAN_INT_SETUP4:
    case MT_NTYPE_NAN_INT_SETUP6:
    case MT_NTYPE_NAN_INT_DESTAB2:
    case MT_NTYPE_NAN_INT_DPAY2:
    case MT_NTYPE_NAN_INT_CLOSE2:
    case MT_NTYPE_NAN_INT_CLOSE4:
    case MT_NTYPE_NAN_INT_CLOSE6:
    case MT_NTYPE_NAN_INT_CLOSE8:
    case MT_NTYPE_NAN_INT_ESTAB3:
    case MT_NTYPE_NAN_INT_ESTAB5:
    case MT_NTYPE_CHN_INT_SETUP:
    case MT_NTYPE_CHN_INT_CLOSE:
    case MT_NTYPE_CHN_INT_REQCLOSE:
    case MT_NTYPE_CHN_INT_CASHOUT:
      return MT_PARTY_INT;
    case MT_NTYPE_MAC_ANY_TRANS:
      return MT_PARTY_IDK;
    default:
      return MT_PARTY_IDK;
  }
}
/** Called when we get a MoneTor cell on circuit circ.
 *  gets the right mt_desc_t and dispatch to the right
 *  payment module
 *
 *  layer_hint allows us to know which relay sent us this cell
 */

MOCK_IMPL(void,
    mt_process_received_relaycell, (circuit_t *circ, relay_header_t* rh,
    relay_pheader_t* rph, crypt_path_t *layer_hint, uint8_t* payload)) {
  (void) rh; //need to refactor
  size_t msg_len = mt_token_get_size_of(rph->pcommand);
  log_debug(LD_MT, "MoneTor: Received cell for token %s with payload length of %d "
      " total message size expected: %ld", mt_token_describe(rph->pcommand), rph->length, msg_len);
  if(ledger_mode(get_options()) || intermediary_mode(get_options()) ||
      server_mode(get_options())) {
    /**
     * We basically have 2 situations - We receive a payment cell over
     * a circuit that we created (an origin_circuit_t), or over
     * a circuti that has been created by someone else */
    if (CIRCUIT_IS_ORCIRC(circ)) {
      // should be circuit built towards us by a client or
      // a relay or an intermediary
      or_circuit_t *orcirc = TO_OR_CIRCUIT(circ);
      /** It is a payment cell over a or-circuit - should be
       * sent a client or a relay - change purpose */
      if (!orcirc->circuit_received_first_payment_cell) {
        // Should be done at the first received payment cell
        // over this circuit
        /* Try to know if the cell comes from a client, a relay
         * or a intermediary */
        mt_party_t party = mt_common_whose_other_edge(rph->pcommand);
        if (ledger_mode(get_options())) {
          circuit_change_purpose(circ, CIRCUIT_PURPOSE_LEDGER);
          mt_cledger_init_desc_and_add(orcirc, party);
        }
        else if (intermediary_mode(get_options())) {
          circuit_change_purpose(circ, CIRCUIT_PURPOSE_INTERMEDIARY);
          mt_cintermediary_init_desc_and_add(orcirc, party);
        }
        else {
          mt_crelay_init_desc_and_add(orcirc, party);
        }
        orcirc->buf = buf_new_with_capacity(RELAY_PPAYLOAD_SIZE);
        orcirc->circuit_received_first_payment_cell = 1;
      }
      /*buffer data if necessary*/
      if (msg_len > RELAY_PPAYLOAD_SIZE) {
        buf_add(orcirc->buf, (char*) payload, rph->length);
        if (buf_datalen(orcirc->buf) == msg_len) {
          /** We now have the full message */
          byte *msg = tor_malloc(msg_len);
          buf_get_bytes(orcirc->buf, (char*) msg, msg_len);
          buf_clear(orcirc->buf);
          if (ledger_mode(get_options())) {
            mt_cledger_process_received_msg(circ, rph->pcommand, msg, msg_len);
          }
          else if (intermediary_mode(get_options())) {
            mt_cintermediary_process_received_msg(circ, rph->pcommand, msg, msg_len);
          }
          else {
            mt_crelay_process_received_msg(circ, rph->pcommand, msg, msg_len);
          }
          tor_free(msg);
        }
        else {
          log_info(LD_MT, "Buffering one received payment cell of type %s"
              " current buf datlen %lu", mt_token_describe(rph->pcommand), buf_datalen(orcirc->buf));
          return;
        }
      }
      else {
        /** No need to buffer */
        tor_assert(rph->length == msg_len);
        if (ledger_mode(get_options())) {
          mt_cledger_process_received_msg(circ, rph->pcommand, payload, rph->length);
        }
        else if (intermediary_mode(get_options())) {
          mt_cintermediary_process_received_msg(circ, rph->pcommand, payload,
            rph->length);
        }
        else {
          mt_crelay_process_received_msg(circ, rph->pcommand, payload, rph->length);
        }
      }
    }
    else if (CIRCUIT_IS_ORIGIN(circ)) {
      // should be a ledger circuit
      origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
      if (msg_len > RELAY_PPAYLOAD_SIZE) {
        buf_add(ocirc->buf, (char*) payload, rph->length);
        if (buf_datalen(ocirc->buf) == msg_len) {
          byte *msg = tor_malloc(msg_len);
          buf_get_bytes(ocirc->buf, (char*) msg, msg_len);
          buf_clear(ocirc->buf);
          if (intermediary_mode(get_options())) {
            mt_cintermediary_process_received_msg(circ, rph->pcommand, msg, msg_len);
          }
          else {
            mt_crelay_process_received_msg(circ, rph->pcommand, msg, msg_len);
          }
          tor_free(msg);
        }
        else {
          log_info(LD_MT, "Buffering one received payment cell of type %hhx"
              " current buf datlen %lu", rph->pcommand, buf_datalen(ocirc->buf));
          return;
        }
      }
      else {
        /** No need to buffer */
        tor_assert(rph->length == msg_len);
        if (ledger_mode(get_options())) {
          mt_cledger_process_received_msg(circ, rph->pcommand, payload,
              rph->length);
        }
        else if (intermediary_mode(get_options())) {
          mt_cintermediary_process_received_msg(circ, rph->pcommand, payload,
              rph->length);
        }
        else {
          mt_crelay_process_received_msg(circ, rph->pcommand, payload,
              rph->length);
        }
      }
    }
  }
  else {
    /* Client mode with one origin circuit */
    if (CIRCUIT_IS_ORIGIN(circ)) {
      if (msg_len > RELAY_PPAYLOAD_SIZE) {
        origin_circuit_t *ocirc = TO_ORIGIN_CIRCUIT(circ);
        if (circ->purpose == CIRCUIT_PURPOSE_C_GENERAL_PAYMENT) {
          // get right ppath
          pay_path_t *ppath = ocirc->ppath;
          crypt_path_t *cpath = ocirc->cpath;
          do {
            cpath = cpath->next;
            ppath = ppath->next;
          } while (cpath != layer_hint);
          /* We have the right hop  -- get the buffer */
          buf_add(ppath->buf, (char*) payload, rph->length);
          if (buf_datalen(ppath->buf) == msg_len) {
            /*We can now process the received message*/
            byte *msg = tor_malloc(msg_len);
            buf_get_bytes(ppath->buf, (char*) msg, msg_len);
            buf_clear(ppath->buf);
            mt_cclient_process_received_msg(ocirc, layer_hint, rph->pcommand, msg, msg_len);
            tor_free(msg);
          }
          else {
            log_info(LD_MT, "Buffering one received payment cell of type %hhx"
                " current buf datalen: %lu", rph->pcommand, buf_datalen(ppath->buf));
            return;
          }
        }
        else if (circ->purpose == CIRCUIT_PURPOSE_C_INTERMEDIARY ||
            circ->purpose == CIRCUIT_PURPOSE_C_LEDGER) {
          buf_add(ocirc->buf, (char*) payload, rph->length);
          if (buf_datalen(ocirc->buf) == msg_len) {
            byte *msg = tor_malloc(msg_len);
            buf_get_bytes(ocirc->buf, (char*) msg, msg_len);
            buf_clear(ocirc->buf);
            mt_cclient_process_received_msg(ocirc, layer_hint, rph->pcommand, msg, msg_len);
            tor_free(msg);
          }
          else {
            log_info(LD_MT, "MoneTor: Buffering one received payment cell of type %hhx"
                " current buf datalen on the intermediary: %lu",
                rph->pcommand, buf_datalen(ocirc->buf));
            return;
          }
        }
        else {
          log_info(LD_MT, "MoneTor: unrecognized purpose %s",
              circuit_purpose_to_string(circ->purpose));
        }
      }
      else {
        /* Yay no need to buffer */
        tor_assert(rph->length == msg_len);
        mt_cclient_process_received_msg(TO_ORIGIN_CIRCUIT(circ), layer_hint, rph->pcommand,
            payload, rph->length);
      }
    }
    else {
      /* defensive prog */
      log_warn(LD_MT, "Receiving a client payment cell on a non-origin circuit. dafuk?");
    }
  }
}

/*
 * Called when we got a peer-level MoneTor cell on this circ. No onion-decryption
 * had to be performed.  cell must contain the plaintext
 */

int mt_process_received_directpaymentcell(circuit_t *circ, cell_t *cell) {

  relay_pheader_t rph;
  relay_pheader_unpack(&rph, cell->payload);
  or_circuit_t *orcirc = NULL;
  origin_circuit_t *oricirc = NULL;
  size_t msg_len = mt_token_get_size_of(rph.pcommand);
  log_info(LD_MT, "MoneTor: Received direct payment %s", mt_token_describe(rph.pcommand));
  if (server_mode(get_options())) {
    orcirc = TO_OR_CIRCUIT(circ);
    if (!orcirc->circuit_received_first_payment_cell) {
        mt_party_t party = mt_common_whose_other_edge(rph.pcommand);
        mt_crelay_init_desc_and_add(orcirc, party);
        orcirc->buf = buf_new_with_capacity(CELL_PPAYLOAD_SIZE);
        orcirc->circuit_received_first_payment_cell = 1;
    }
    if (msg_len > CELL_PPAYLOAD_SIZE) {
      buf_add(orcirc->buf,
          (char*)cell->payload+RELAY_PHEADER_SIZE, rph.length);
      if (buf_datalen(orcirc->buf) == msg_len) {
        /** We now have the full message */
        byte *msg = tor_malloc_zero(msg_len);
        buf_get_bytes(orcirc->buf, (char*) msg, msg_len);
        buf_clear(orcirc->buf);
        mt_crelay_process_received_msg(circ, rph.pcommand, msg, msg_len);
        tor_free(msg);
      }
      else {
        log_info(LD_MT, "Buffering one received payment cell of type %hhx"
            " current buf datlen %lu", rph.pcommand, buf_datalen(orcirc->buf));
        return 0;
      }
    }
    else {
      /** No need to buffer */
      tor_assert(rph.length == msg_len);
      mt_crelay_process_received_msg(circ, rph.pcommand,
          cell->payload+RELAY_PHEADER_SIZE, rph.length);
    }
  }
  else {
    /* Should in client mode with an origin circuit */
    if (CIRCUIT_IS_ORIGIN(circ)) {
      /* everything's ok, let's proceed */
      oricirc = TO_ORIGIN_CIRCUIT(circ);
      if (msg_len > CELL_PPAYLOAD_SIZE){
        buf_add(oricirc->ppath->buf,
            (char*)cell->payload+RELAY_PHEADER_SIZE, rph.length);
        if (buf_datalen(oricirc->buf) == msg_len) {
          byte *msg = tor_malloc_zero(msg_len);
          buf_get_bytes(oricirc->ppath->buf, (char*) msg, msg_len);
          buf_clear(oricirc->ppath->buf);
          mt_cclient_process_received_msg(oricirc, oricirc->cpath, rph.pcommand,
              msg, msg_len);
          tor_free(msg);
        }
        else {
          log_info(LD_MT, "Buffering one received payment cell of type %hhx"
              " current buf datlen %lu", rph.pcommand, buf_datalen(oricirc->buf));
          return 0;
        }
      }
      else {
        /** No need to buffer */
        tor_assert(rph.length == msg_len);
        mt_cclient_process_received_msg(oricirc, oricirc->cpath, rph.pcommand,
            cell->payload+RELAY_PHEADER_SIZE, rph.length);
      }
    }
    else {
      return -1;
    }
  }
  /* if we reach this, everything is ok */
  return 0;
}

/** Interface to the payment module to send a payment cell.
 *  This function dispaches to the right controller.
 */

MOCK_IMPL(int, mt_send_message, (mt_desc_t *desc, mt_ntype_t type,
      byte* msg, int size)) {

  log_info(LD_MT, "MoneTor: Sending %s to %s %" PRIu64 ".%" PRIu64 "",
	   mt_token_describe(type), mt_party_describe(desc->party),
	   desc->id[0], desc->id[1]);

  switch (type) {
    uint8_t command;
    /* sending Client related message */
    case MT_NTYPE_NAN_CLI_DESTAB1:
    case MT_NTYPE_NAN_CLI_DPAY1:
      command = CELL_PAYMENT;
      return mt_cclient_send_message(desc, command, type, msg, size);
    case MT_NTYPE_MIC_CLI_PAY1:
    case MT_NTYPE_MIC_CLI_PAY3:
    case MT_NTYPE_MIC_CLI_PAY5:
    case MT_NTYPE_NAN_CLI_SETUP1:
    case MT_NTYPE_NAN_CLI_SETUP3:
    case MT_NTYPE_NAN_CLI_SETUP5:
    case MT_NTYPE_NAN_CLI_ESTAB1:
    case MT_NTYPE_NAN_CLI_PAY1:
    case MT_NTYPE_NAN_CLI_REQCLOSE1:
      command = RELAY_COMMAND_MT;
      return mt_cclient_send_message(desc, command, type, msg, size);
    /* Sending relay related message */
    case MT_NTYPE_MIC_REL_PAY2:
    case MT_NTYPE_MIC_REL_PAY6:
    case MT_NTYPE_NAN_REL_ESTAB2:
    case MT_NTYPE_NAN_REL_ESTAB4:
    case MT_NTYPE_NAN_REL_ESTAB6:
    case MT_NTYPE_NAN_REL_PAY2:
    case MT_NTYPE_NAN_REL_REQCLOSE2:
      command = RELAY_COMMAND_MT;
      return mt_crelay_send_message(desc, command, type, msg, size);
    /* Sending to intermediary from client or server */
    case MT_NTYPE_CHN_END_ESTAB1:
    case MT_NTYPE_CHN_END_ESTAB3:
    case MT_NTYPE_NAN_END_CLOSE1:
    case MT_NTYPE_NAN_END_CLOSE3:
    case MT_NTYPE_NAN_END_CLOSE5:
    case MT_NTYPE_NAN_END_CLOSE7:
    case MT_NTYPE_CHN_END_SETUP:
    case MT_NTYPE_CHN_END_CLOSE:
    case MT_NTYPE_CHN_END_CASHOUT:
      command = RELAY_COMMAND_MT;
      /* check server mode*/
      if (server_mode(get_options())) {
        return mt_crelay_send_message(desc, command, type, msg, size);
      }
      else {
        return mt_cclient_send_message(desc, command, type, msg, size);
      }
    /* Sending from authority */
    case MT_NTYPE_MAC_AUT_MINT:
      if (ledger_mode(get_options())) {
        return mt_cledger_send_message(desc, type, msg, size);
      }
      else {
        log_warn(LD_MT, "MoneTor: Cannot handle type %s from a ledger", mt_token_describe(type));
      }
      break;
    /* Sending from intermediary */
    case MT_NTYPE_CHN_INT_ESTAB2:
    case MT_NTYPE_CHN_INT_ESTAB4:
    case MT_NTYPE_MIC_INT_PAY4:
    case MT_NTYPE_MIC_INT_PAY7:
    case MT_NTYPE_MIC_INT_PAY8:
    case MT_NTYPE_NAN_INT_SETUP2:
    case MT_NTYPE_NAN_INT_SETUP4:
    case MT_NTYPE_NAN_INT_SETUP6:
    case MT_NTYPE_NAN_INT_CLOSE2:
    case MT_NTYPE_NAN_INT_CLOSE4:
    case MT_NTYPE_NAN_INT_CLOSE6:
    case MT_NTYPE_NAN_INT_CLOSE8:
    case MT_NTYPE_NAN_INT_ESTAB3:
    case MT_NTYPE_NAN_INT_ESTAB5:
    case MT_NTYPE_CHN_INT_SETUP:
    case MT_NTYPE_CHN_INT_CLOSE:
    case MT_NTYPE_CHN_INT_REQCLOSE:
    case MT_NTYPE_CHN_INT_CASHOUT:
      if (intermediary_mode(get_options())) {
        return mt_cintermediary_send_message(desc, type, msg, size);
      }
      else if(server_mode(get_options())){
        command = RELAY_COMMAND_MT;
        return mt_crelay_send_message(desc, command, type, msg, size);
      }
      else {
        log_warn(LD_MT, "MoneTor: Cannot handle type %s from anything else than an intermediary or a guard", mt_token_describe(type));
      }
      break;
      /* Sending from any of them */
    case MT_NTYPE_NAN_INT_DESTAB2:
    case MT_NTYPE_NAN_INT_DPAY2:
        if (server_mode(get_options())) {
          command = CELL_PAYMENT;
          return mt_crelay_send_message(desc, command, type, msg, size);
        }
        else {
          log_warn(LD_MT, "MoneTor: Cannot handle type %s from anything else than a relay", mt_token_describe(type));
        }
        break;
    case MT_NTYPE_MAC_ANY_TRANS:
    case MT_NTYPE_ANY_LED_CONFIRM:
    case MT_NTYPE_MAC_LED_DATA:
    case MT_NTYPE_CHN_LED_DATA:
    case MT_NTYPE_CHN_LED_QUERY:
      command = RELAY_COMMAND_MT;
      if (ledger_mode(get_options())) {
        return mt_cledger_send_message(desc, type, msg, size);
      }
      else if (intermediary_mode(get_options())) {
        return mt_cintermediary_send_message(desc, type, msg, size);
      }
      else if (server_mode(get_options())) {
        return mt_crelay_send_message(desc, command, type, msg, size);
      }
      else {
        return mt_cclient_send_message(desc, command, type, msg, size);
      }

    default:
      log_warn(LD_MT, "MoneTor - Unrecognized type %s", mt_token_describe(type));
      return -1;
  }
  log_info(LD_MT, "MoneTor: We should not reach this.. we return -2");
  return -2;
}

/**
 * Direct Payment cells sent from Client or from a guard
 * relay. Wrote here to avoid code duplication within
 * mt_cclient and mt_crelay. They both should call this function
 * when sending a direct payment cell.
 */

int mt_common_send_direct_cell_payment(circuit_t *circ, mt_ntype_t type,
    byte *msg, int size, cell_direction_t direction) {

  or_circuit_t *orcirc = NULL;
  cell_t cell;
  relay_pheader_t rph;
  memset(&cell, 0, sizeof(cell_t));
  memset(&rph, 0, sizeof(relay_pheader_t));
  if (direction == CELL_DIRECTION_OUT) {
    cell.circ_id = circ->n_circ_id;
  }
  else {
    orcirc = TO_OR_CIRCUIT(circ);
    cell.circ_id = orcirc->p_circ_id;
  }
  cell.command = CELL_PAYMENT;
  rph.pcommand = type;
  int nbr_cells;
  if (size % CELL_PPAYLOAD_SIZE == 0)
    nbr_cells = size/CELL_PPAYLOAD_SIZE;
  else
    nbr_cells = size/CELL_PPAYLOAD_SIZE + 1;
  int remaining_payload = size;
  for (int i = 0; i < nbr_cells; i++) {
    if (remaining_payload <= CELL_PPAYLOAD_SIZE) {
      rph.length = remaining_payload;
    }
    else {
      rph.length = CELL_PPAYLOAD_SIZE;
    }
    direct_pheader_pack(cell.payload, &rph);
    memcpy(cell.payload+RELAY_PHEADER_SIZE, msg+i*CELL_PPAYLOAD_SIZE, rph.length);
    remaining_payload -= rph.length;
    log_info(LD_MT, "MoneTor: Adding cell payment %s to queue", mt_token_describe(rph.pcommand));
    if (direction == CELL_DIRECTION_OUT) {
      circuit_log_path(LOG_INFO, LD_MT, TO_ORIGIN_CIRCUIT(circ));
      cell_queue_append_packed_copy(NULL, &circ->n_chan_cells, 0, &cell,
          circ->n_chan->wide_circ_ids, 0);
    }
    else {
      cell_queue_append_packed_copy(NULL, &orcirc->p_chan_cells, 0, &cell,
          orcirc->p_chan->wide_circ_ids, 0);
    }
  }
  tor_assert(remaining_payload == 0);
  update_circuit_on_cmux(circ, direction);
  if (direction == CELL_DIRECTION_OUT && circ->n_chan) {
    scheduler_channel_has_waiting_cells(circ->n_chan);
  }
  else if (orcirc->p_chan){
    scheduler_channel_has_waiting_cells(orcirc->p_chan);
  }
  else {
    log_warn(LD_MT, "MoneTor: circ->n_chan or orcirc->p_chan is null?");
  }
  return 0;
}

/**
 * Called to send a intermediary descriptor to a relay. This is sent by
 * a client.
 */
MOCK_IMPL(int, mt_send_message_multidesc, (mt_desc_t *desc1, mt_desc_t* desc2,
      mt_ntype_t type, byte* msg, int size)) {

  if (ledger_mode(get_options()) || intermediary_mode(get_options()) ||
      server_mode(get_options())) {
    log_info(LD_BUG, "MoneTor: this function should only be called on a client");
    return -1;
  }

  log_info(LD_MT, "MoneTor: Sending %s to %s %" PRIu64 ".%" PRIu64 " | %" PRIu64 ".%" PRIu64 "",
	   mt_token_describe(type), mt_party_describe(desc1->party),
	   desc1->id[0], desc1->id[1], desc2->id[0], desc2->id[1]);

  return mt_cclient_send_message_multidesc(desc1, desc2, type, msg, size);
}

MOCK_IMPL(int, mt_paymod_signal, (mt_signal_t signal, mt_desc_t *desc)){

  log_info(LD_MT, "MoneTor: received signal %s for desc %s",
      mt_signal_describe(signal), mt_desc_describe(desc));
  if (ledger_mode(get_options())) {
    return mt_cledger_paymod_signal(signal, desc);
  }
  else if (intermediary_mode(get_options())) {
    return mt_cintermediary_paymod_signal(signal, desc);
  }
  else if (server_mode(get_options())) {
    return mt_crelay_paymod_signal(signal, desc);
  }
  else {
    return mt_cclient_paymod_signal(signal, desc);
  }
}

/**
 * Mark the payment channel for close and try to accomplish
 * a nanopayment close. If abort is true, then we just
 * abort the protocol
 *
 * This function should call circuit_mark_for_close() if
 * no control cell to close the circuit has to be sent
 *
 */

void circuit_mark_payment_channel_for_close(circuit_t *circ, int abort, int reason) {
  log_info(LD_MT, "MoneTor: Trying to close a circuit that might have a payment channel"
      " associated.");
  if (ledger_mode(get_options())) {
    mt_cledger_mark_payment_channel_for_close(circ, abort, reason);
  }
  else if (intermediary_mode(get_options())) {
    mt_cintermediary_mark_payment_channel_for_close(circ, abort, reason);
  }
  else if (server_mode(get_options())) {
    mt_crelay_mark_payment_channel_for_close(circ, abort, reason);
  }
  else {
    mt_cclient_mark_payment_channel_for_close(circ, abort, reason);
  }
}
