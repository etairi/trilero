#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "relic/relic.h"
#include "zmq.h"
#include "bob.h"
#include "types.h"
#include "util.h"

unsigned PROMISE_COMPLETED;
unsigned PUZZLE_SHARED;
unsigned PUZZLE_SOLVED;

int get_message_type(char *key) {
  for (size_t i = 0; i < TOTAL_MESSAGES; i++) {
    symstruct_t sym = msg_lookuptable[i];
    if (strcmp(sym.key, key) == 0) {
      return sym.code;
    }
  }
  return -1;
}

msg_handler_t get_message_handler(char *key) {
  switch (get_message_type(key))
  {
    case PROMISE_INIT_DONE:
      return promise_init_done_handler;
    case PROMISE_SIGN_DONE:
      return promise_sign_done_handler;

    case PROMISE_END_DONE:
      return promise_end_done_handler;

    case PUZZLE_SHARE_DONE:
      return puzzle_share_done_handler;

    case PUZZLE_SOLUTION_SHARE:
      return puzzle_solution_share_handler;

    default:
      fprintf(stderr, "Error: invalid message type.\n");
      exit(1);
  }
}

int handle_message(bob_state_t state, void *socket, zmq_msg_t message) {
  int result_status = RLC_OK;

  message_t msg;
  message_null(msg);

  TRY {
    printf("Received message size: %ld bytes\n", zmq_msg_size(&message));
    deserialize_message(&msg, (uint8_t *) zmq_msg_data(&message));

    printf("Executing %s...\n", msg->type);
    msg_handler_t msg_handler = get_message_handler(msg->type);
    if (msg_handler(state, socket, msg->data) != RLC_OK) {
      THROW(ERR_CAUGHT);
    }
    printf("Finished executing %s.\n\n", msg->type);
  } CATCH_ANY {
    result_status = RLC_ERR;
  } FINALLY {
    if (msg != NULL) message_free(msg);
  }

  return result_status;
}

int receive_message(bob_state_t state, void *socket) {
  int result_status = RLC_OK;

  zmq_msg_t message;

  TRY {
    int rc = zmq_msg_init(&message);
    if (rc) {
      fprintf(stderr, "Error: could not initialize the message.\n");
      THROW(ERR_CAUGHT);
    }

    rc = zmq_msg_recv(&message, socket, ZMQ_DONTWAIT);
    if (rc != -1 && handle_message(state, socket, message) != RLC_OK) THROW(ERR_CAUGHT);
  } CATCH_ANY {
    result_status = RLC_ERR;
  } FINALLY {
    zmq_msg_close(&message);
  }

  return result_status;
}

int promise_init(void *socket) {
  int result_status = RLC_OK;
  uint8_t *serialized_message = NULL;
  
  message_t promise_init_msg;
  message_null(promise_init_msg);

  TRY {
    // Build and define the message.
    char *msg_type = "promise_init";
    const unsigned msg_type_length = (unsigned) strlen(msg_type) + 1;
    const unsigned msg_data_length = 0;
    const int total_msg_length = msg_type_length + msg_data_length + (2 * sizeof(unsigned));
    message_new(promise_init_msg, msg_type_length, msg_data_length);
    
    // Serialize the message.
    memcpy(promise_init_msg->type, msg_type, msg_type_length);
    serialize_message(&serialized_message, promise_init_msg, msg_type_length, msg_data_length);

    // Send the message.
    zmq_msg_t promise_init;
    int rc = zmq_msg_init_size(&promise_init, total_msg_length);
    if (rc) {
      fprintf(stderr, "Error: could not initialize the message (%s).\n", msg_type);
      THROW(ERR_CAUGHT);
    }

    memcpy(zmq_msg_data(&promise_init), serialized_message, total_msg_length);
    rc = zmq_msg_send(&promise_init, socket, ZMQ_DONTWAIT);
    if (rc != total_msg_length) {
      fprintf(stderr, "Error: could not send the message (%s).\n", msg_type);
      THROW(ERR_CAUGHT);
    }
  } CATCH_ANY {
    result_status = RLC_ERR;
  } FINALLY {
    message_free(promise_init_msg);
    if (serialized_message != NULL) free(serialized_message);
  }

  return result_status;
}

int promise_init_done_handler(bob_state_t state, void *socket, uint8_t *data) {
  if (state == NULL || data == NULL) {
    THROW(ERR_NO_VALID);
  }

  int result_status = RLC_OK;

  uint8_t *serialized_message = NULL;
  message_t promise_sign_msg;

  bn_t q;
  zk_proof_t pi_alpha;
  zk_proof_t pi_1_prime;

  bn_null(q);
  zk_proof_null(pi_alpha);
  zk_proof_null(pi_1_prime);
  message_null(promise_sign_msg);

  TRY {
    bn_new(q);
    zk_proof_new(pi_alpha);
    zk_proof_new(pi_1_prime);

    // Deserialize the data from the message.
    ec_read_bin(state->g_to_the_alpha, data, RLC_EC_SIZE_COMPRESSED);
    bn_read_bin(state->com->c, data + RLC_EC_SIZE_COMPRESSED, RLC_BN_SIZE);
    ec_read_bin(state->com->r, data + RLC_EC_SIZE_COMPRESSED + RLC_BN_SIZE, RLC_EC_SIZE_COMPRESSED);
    ec_read_bin(pi_alpha->a, data + (2 * RLC_EC_SIZE_COMPRESSED) + RLC_BN_SIZE, RLC_EC_SIZE_COMPRESSED);
    bn_read_bin(pi_alpha->z, data + (3 * RLC_EC_SIZE_COMPRESSED) + RLC_BN_SIZE, RLC_BN_SIZE);
    bn_read_bin(state->ctx_alpha, data + (3 * RLC_EC_SIZE_COMPRESSED) + (2 * RLC_BN_SIZE), RLC_PAILLIER_CTX_SIZE);

    // Verify ZK proof.
    if (zk_dlog_verify(pi_alpha, state->g_to_the_alpha) != RLC_OK) {
      THROW(ERR_CAUGHT);
    }

    ec_curve_get_ord(q);

    bn_rand_mod(state->k_1_prime, q);
    ec_mul_gen(state->R_1_prime, state->k_1_prime);

    if (zk_dlog_prove(pi_1_prime, state->R_1_prime, state->k_1_prime) != RLC_OK) {
      THROW(ERR_CAUGHT);
    }

    // Build and define the message.
    char *msg_type = "promise_sign";
    const unsigned msg_type_length = (unsigned) strlen(msg_type) + 1;
    const unsigned msg_data_length = (2 * RLC_EC_SIZE_COMPRESSED) + RLC_BN_SIZE;
    const int total_msg_length = msg_type_length + msg_data_length + (2 * sizeof(unsigned));
    message_new(promise_sign_msg, msg_type_length, msg_data_length);

    // Serialize the data for the message.
    ec_write_bin(promise_sign_msg->data, RLC_EC_SIZE_COMPRESSED, state->R_1_prime, 1);
    ec_write_bin(promise_sign_msg->data + RLC_EC_SIZE_COMPRESSED, RLC_EC_SIZE_COMPRESSED, pi_1_prime->a, 1);
    bn_write_bin(promise_sign_msg->data + (2 * RLC_EC_SIZE_COMPRESSED), RLC_BN_SIZE, pi_1_prime->z);

    memcpy(promise_sign_msg->type, msg_type, msg_type_length);
    serialize_message(&serialized_message, promise_sign_msg, msg_type_length, msg_data_length);

    // Send the message.
    zmq_msg_t promise_sign;
    int rc = zmq_msg_init_size(&promise_sign, total_msg_length);
    if (rc) {
      fprintf(stderr, "Error: could not initialize the message (%s).\n", msg_type);
      THROW(ERR_CAUGHT);
    }

    memcpy(zmq_msg_data(&promise_sign), serialized_message, total_msg_length);
    rc = zmq_msg_send(&promise_sign, socket, ZMQ_DONTWAIT);
    if (rc != total_msg_length) {
      fprintf(stderr, "Error: could not send the message (%s).\n", msg_type);
      THROW(ERR_CAUGHT);
    }
  } CATCH_ANY {
    result_status = RLC_ERR;
  } FINALLY {
    bn_free(q);
    zk_proof_free(pi_alpha);
    zk_proof_free(pi_1_prime);
    if (promise_sign_msg != NULL) message_free(promise_sign_msg);
    if (serialized_message != NULL) free(serialized_message);
  }

  return result_status;
}

int promise_sign_done_handler(bob_state_t state, void *socket, uint8_t *data) {
  if (state == NULL || data == NULL) {
    THROW(ERR_NO_VALID);
  }

  int result_status = RLC_OK;

  unsigned tx_len = sizeof(tx);
  uint8_t *tx_msg = malloc(tx_len + RLC_FC_BYTES);
  uint8_t hash[RLC_MD_LEN];
  uint8_t *serialized_message = NULL;
  message_t promise_end_msg;

  bn_t q, r, x, neg_sk, neg_e_prime;
  ec_t com_x, g_to_the_s_2_prime;
  ec_t g_to_the_neg_sk, pk_times_g_to_the_neg_sk;
  ec_t g_to_the_x_2_minus_e_prime;
  ec_t R_2_prime_time_g_to_the_x_2_minus_e_prime;
  ec_t R_2_prime, R_prime;
  bn_t s_2_prime, e_prime, s_1_prime;
  zk_proof_t pi_2_prime;

  ec_null(com_x);
  ec_null(g_to_the_s_2_prime);
  ec_null(g_to_the_neg_sk);
  ec_null(pk_times_g_to_the_neg_sk);
  ec_null(g_to_the_x_2_minus_e_prime);
  ec_null(R_2_prime_time_g_to_the_x_2_minus_e_prime);
  bn_null(q);
  bn_null(r);
  bn_null(x);
  bn_null(neg_sk);
  bn_null(neg_e_prime);
  bn_null(e_prime);
  bn_null(s_1_prime);
  ec_null(R_prime);
  ec_null(R_2_prime);
  bn_null(s_2_prime);
  zk_proof_null(pi_2_prime);

  TRY {
    ec_new(com_x);
    ec_new(g_to_the_s_2_prime);
    ec_new(g_to_the_neg_sk);
    ec_new(pk_times_g_to_the_neg_sk);
    ec_new(g_to_the_x_2_minus_e_prime);
    ec_new(R_2_prime_time_g_to_the_x_2_minus_e_prime);
    bn_new(q);
    bn_new(r);
    bn_new(x);
    bn_new(neg_sk);
    bn_new(neg_e_prime);
    bn_new(e_prime);
    bn_new(s_1_prime);
    ec_new(R_prime);
    ec_new(R_2_prime);
    bn_new(s_2_prime);
    zk_proof_new(pi_2_prime);

    // Deserialize the data from the message.
    ec_read_bin(R_2_prime, data, RLC_EC_SIZE_COMPRESSED);
    ec_read_bin(pi_2_prime->a, data + RLC_EC_SIZE_COMPRESSED, RLC_EC_SIZE_COMPRESSED);
    bn_read_bin(pi_2_prime->z, data + (2 * RLC_EC_SIZE_COMPRESSED), RLC_BN_SIZE);
    bn_read_bin(s_2_prime, data + (2 * RLC_EC_SIZE_COMPRESSED) + RLC_BN_SIZE, RLC_BN_SIZE);

    // Verify the commitment and ZK proof.
    ec_add(com_x, R_2_prime, pi_2_prime->a);
    if (decommit(state->com, com_x) != RLC_OK) {
      THROW(ERR_CAUGHT);
    }

    if (zk_dlog_verify(pi_2_prime, R_2_prime) != RLC_OK) {
      THROW(ERR_CAUGHT);
    }

    // Compute the half Schnorr signature.
    ec_add(R_prime, state->R_1_prime, R_2_prime);
    ec_norm(R_prime, R_prime);
    ec_add(R_prime, R_prime, state->g_to_the_alpha);
    ec_norm(R_prime, R_prime);

    ec_curve_get_ord(q);
    ec_get_x(x, R_prime);
    bn_mod(r, x, q);
    if (bn_is_zero(r)) {
      THROW(ERR_CAUGHT);
    }

		memcpy(tx_msg, tx, tx_len);
		bn_write_bin(tx_msg + tx_len, RLC_FC_BYTES, r);
		md_map(hash, tx_msg, tx_len + RLC_FC_BYTES);

		if (8 * RLC_MD_LEN > bn_bits(q)) {
			tx_len = RLC_CEIL(bn_bits(q), 8);
			bn_read_bin(e_prime, hash, tx_len);
			bn_rsh(state->e_prime, e_prime, 8 * RLC_MD_LEN - bn_bits(q));
		} else {
			bn_read_bin(state->e_prime, hash, RLC_MD_LEN);
		}

		bn_mod(state->e_prime, state->e_prime, q);

    // Check correctness of the partial signature received.
    ec_mul_gen(g_to_the_s_2_prime, s_2_prime);
    bn_neg(neg_sk, state->keys->ec_sk->sk);
    ec_mul_gen(g_to_the_neg_sk, neg_sk);
    ec_add(pk_times_g_to_the_neg_sk, state->keys->ec_pk->pk, g_to_the_neg_sk);
    bn_neg(neg_e_prime, state->e_prime);
    ec_mul(g_to_the_x_2_minus_e_prime, pk_times_g_to_the_neg_sk, neg_e_prime);
    ec_add(R_2_prime_time_g_to_the_x_2_minus_e_prime, R_2_prime, g_to_the_x_2_minus_e_prime);
    
    if (ec_cmp(g_to_the_s_2_prime, R_2_prime_time_g_to_the_x_2_minus_e_prime) != RLC_EQ) {
      THROW(ERR_CAUGHT);
    }

		bn_mul(s_1_prime, state->keys->ec_sk->sk, state->e_prime);
		bn_mod(s_1_prime, s_1_prime, q);
		bn_sub(s_1_prime, q, s_1_prime);
		bn_add(s_1_prime, s_1_prime, state->k_1_prime);
		bn_mod(s_1_prime, s_1_prime, q);

    // Compute the "almost" signature.
    bn_add(state->s_prime, s_1_prime, s_2_prime);
    bn_mod(state->s_prime, state->s_prime, q);
    
    // Build and define the message.
    char *msg_type = "promise_end";
    const unsigned msg_type_length = (unsigned) strlen(msg_type) + 1;
    const unsigned msg_data_length = RLC_BN_SIZE;
    const int total_msg_length = msg_type_length + msg_data_length + (2 * sizeof(unsigned));
    message_new(promise_end_msg, msg_type_length, msg_data_length);

    // Serialize the data for the message.
    bn_write_bin(promise_end_msg->data, RLC_BN_SIZE, state->s_prime);

    memcpy(promise_end_msg->type, msg_type, msg_type_length);
    serialize_message(&serialized_message, promise_end_msg, msg_type_length, msg_data_length);

    // Send the message.
    zmq_msg_t promise_end;
    int rc = zmq_msg_init_size(&promise_end, total_msg_length);
    if (rc) {
      fprintf(stderr, "Error: could not initialize the message (%s).\n", msg_type);
      THROW(ERR_CAUGHT);
    }

    memcpy(zmq_msg_data(&promise_end), serialized_message, total_msg_length);
    rc = zmq_msg_send(&promise_end, socket, ZMQ_DONTWAIT);
    if (rc != total_msg_length) {
      fprintf(stderr, "Error: could not send the message (%s).\n", msg_type);
      THROW(ERR_CAUGHT);
    }
  } CATCH_ANY {
    result_status = RLC_ERR;
  } FINALLY {
    ec_free(com_x);
    ec_free(g_to_the_s_2_prime);
    ec_free(g_to_the_neg_sk);
    ec_free(pk_times_g_to_the_neg_sk);
    ec_free(g_to_the_x_2_minus_e_prime);
    ec_free(R_2_prime_time_g_to_the_x_2_minus_e_prime);
    bn_free(q);
    bn_free(r);
    bn_free(x);
    bn_free(neg_sk);
    bn_free(neg_e_prime);
    bn_free(e_prime);
    bn_free(s_1_prime);
    ec_free(R_prime);
    ec_free(R_2_prime);
    bn_free(s_2_prime);
    zk_proof_free(pi_2_prime);
    free(tx_msg);
    if (serialized_message != NULL) free(serialized_message);
    if (promise_end_msg != NULL) message_free(promise_end_msg);
  }

  return result_status;
}

int promise_end_done_handler(bob_state_t state, void *socket, uint8_t *data) {
  if (state == NULL || data == NULL) {
    THROW(ERR_NO_VALID);
  }

  int result_status = RLC_OK;

  TRY {
    PROMISE_COMPLETED = 1;
  } CATCH_ANY {
    result_status = RLC_ERR;
  }

  return result_status;
}

int puzzle_share(bob_state_t state, void *socket) {
  if (state == NULL) {
    THROW(ERR_NO_VALID);
  }
  
  int result_status = RLC_OK;

  uint8_t *serialized_message = NULL;
  uint8_t in[RLC_PAILLIER_CTX_SIZE];
  uint8_t out[RLC_PAILLIER_CTX_SIZE];
  int in_len = bn_size_bin(state->tumbler_paillier_pk->pk);
  int out_len = RLC_PAILLIER_CTX_SIZE;
  
  message_t puzzle_share_msg;
  message_null(puzzle_share_msg);

  bn_t q, s, ctx_beta, ctx_alpha_plus_beta;
  ec_t g_to_the_beta;
  ec_t g_to_the_alpha_plus_beta;

  bn_null(q);
  bn_null(s);
  bn_null(ctx_beta);
  bn_null(ctx_alpha_plus_beta);
  ec_null(g_to_the_beta);
  ec_null(g_to_the_alpha_plus_beta);

  TRY {
    bn_new(q);
    bn_new(s);
    bn_new(ctx_beta);
    bn_new(ctx_alpha_plus_beta);
    ec_new(g_to_the_beta);
    ec_new(g_to_the_alpha_plus_beta);

    ec_curve_get_ord(q);

    // Randomize the promise challenge.
    bn_rand_mod(state->beta, q);
    ec_mul_gen(g_to_the_beta, state->beta);
    ec_add(g_to_the_alpha_plus_beta, state->g_to_the_alpha, g_to_the_beta);
    ec_norm(g_to_the_alpha_plus_beta, g_to_the_alpha_plus_beta);

    // Homomorphically randomize the challenge ciphertext.
    bn_write_bin(in, in_len, state->beta);
		if (cp_phpe_enc(out, &out_len, in, in_len, state->tumbler_paillier_pk->pk) != RLC_OK) {
      THROW(ERR_CAUGHT);
    }
    bn_read_bin(ctx_beta, out, out_len);

    bn_sqr(s, state->tumbler_paillier_pk->pk);
		bn_mul(ctx_alpha_plus_beta, state->ctx_alpha, ctx_beta);
		bn_mod(ctx_alpha_plus_beta, ctx_alpha_plus_beta, s);

    // Build and define the message.
    char *msg_type = "puzzle_share";
    const unsigned msg_type_length = (unsigned) strlen(msg_type) + 1;
    const unsigned msg_data_length = RLC_EC_SIZE_COMPRESSED + RLC_PAILLIER_CTX_SIZE;
    const int total_msg_length = msg_type_length + msg_data_length + (2 * sizeof(unsigned));
    message_new(puzzle_share_msg, msg_type_length, msg_data_length);

    // Serialize the data for the message.
    ec_write_bin(puzzle_share_msg->data, RLC_EC_SIZE_COMPRESSED, g_to_the_alpha_plus_beta, 1);
    bn_write_bin(puzzle_share_msg->data + RLC_EC_SIZE_COMPRESSED, RLC_PAILLIER_CTX_SIZE, ctx_alpha_plus_beta);
    
    // Serialize the message.
    memcpy(puzzle_share_msg->type, msg_type, msg_type_length);
    serialize_message(&serialized_message, puzzle_share_msg, msg_type_length, msg_data_length);

    // Send the message.
    zmq_msg_t puzzle_share;
    int rc = zmq_msg_init_size(&puzzle_share, total_msg_length);
    if (rc) {
      fprintf(stderr, "Error: could not initialize the message (%s).\n", msg_type);
      THROW(ERR_CAUGHT);
    }

    memcpy(zmq_msg_data(&puzzle_share), serialized_message, total_msg_length);
    rc = zmq_msg_send(&puzzle_share, socket, ZMQ_DONTWAIT);
    if (rc != total_msg_length) {
      fprintf(stderr, "Error: could not send the message (%s).\n", msg_type);
      THROW(ERR_CAUGHT);
    }
  } CATCH_ANY {
    result_status = RLC_ERR;
  } FINALLY {
    bn_free(q);
    bn_free(s);
    bn_free(ctx_beta);
    bn_free(ctx_alpha_plus_beta);
    ec_free(g_to_the_beta);
    ec_free(g_to_the_alpha_plus_beta);
    if (puzzle_share_msg != NULL) message_free(puzzle_share_msg);
    if (serialized_message != NULL) free(serialized_message);
  }

  return result_status;
}

int puzzle_share_done_handler(bob_state_t state, void *socket, uint8_t *data) {
  if (state == NULL || data == NULL) {
    THROW(ERR_NO_VALID);
  }

  int result_status = RLC_OK;

  TRY {
    PUZZLE_SHARED = 1;
  } CATCH_ANY {
    result_status = RLC_ERR;
  }

  return result_status;
}

int puzzle_solution_share_handler(bob_state_t state, void *socet, uint8_t *data) {
  if (state == NULL || data == NULL) {
    THROW(ERR_NO_VALID);
  }

  int result_status = RLC_OK;
  int verif_status = RLC_ERR;

  unsigned tx_len = sizeof(tx);
  uint8_t *tx_msg = malloc(tx_len + RLC_FC_BYTES);
  uint8_t hash[RLC_MD_LEN];

  bn_t q, alpha, alpha_hat;
  bn_t ev, rv;
  ec_t p;

  bn_null(q);
  bn_null(alpha);
  bn_null(alpha_hat);
  bn_null(ev);
  bn_null(rv);
  ec_null(p);

  TRY {
    bn_new(q);
    bn_new(alpha);
    bn_new(alpha_hat);
    bn_new(ev);
    bn_new(rv);
    ec_new(p);
    
    // Deserialize the data from the message.
    bn_read_bin(alpha_hat, data, RLC_BN_SIZE);

    ec_curve_get_ord(q);

    // Extract the secret alpha.
    bn_sub(alpha, alpha_hat, state->beta);
    bn_mod(alpha, alpha, q);

    // Complete the "almost" signature.
    bn_add(state->s_prime, state->s_prime, alpha);
    bn_mod(state->s_prime, state->s_prime, q);

    // Verify the completed signature.
    if (bn_sign(state->e_prime) == RLC_POS && bn_sign(state->s_prime) == RLC_POS && !bn_is_zero(state->s_prime)) {
			if (bn_cmp(state->e_prime, q) == RLC_LT && bn_cmp(state->s_prime, q) == RLC_LT) {
				ec_mul_sim_gen(p, state->s_prime, state->keys->ec_pk->pk, state->e_prime);
				ec_get_x(rv, p);

				bn_mod(rv, rv, q);

				memcpy(tx_msg, tx, tx_len);
				bn_write_bin(tx_msg + tx_len, RLC_FC_BYTES, rv);
				md_map(hash, tx_msg, tx_len + RLC_FC_BYTES);

				if (8 * RLC_MD_LEN > bn_bits(q)) {
					tx_len = RLC_CEIL(bn_bits(q), 8);
					bn_read_bin(ev, hash, tx_len);
					bn_rsh(ev, ev, 8 * RLC_MD_LEN - bn_bits(q));
				} else {
					bn_read_bin(ev, hash, RLC_MD_LEN);
				}

				bn_mod(ev, ev, q);

				verif_status = dv_cmp_const(ev->dp, state->e_prime->dp, RLC_MIN(ev->used, state->e_prime->used));
				verif_status = (verif_status == RLC_NE ? RLC_ERR : RLC_OK);

				if (ev->used != state->e_prime->used) {
					verif_status = RLC_ERR;
				}

        if (verif_status != RLC_OK) {
          THROW(ERR_CAUGHT);
        }
			}
		}

    PUZZLE_SOLVED = 1;
  } CATCH_ANY {
    result_status = RLC_ERR;
  } FINALLY {
    bn_free(q);
    bn_free(alpha)
    bn_free(alpha_hat);
    bn_free(ev);
    bn_free(rv);
    bn_free(p);
  }

  return result_status;
}

int main(void)
{
  init();
  int result_status = RLC_OK;
  PROMISE_COMPLETED = 0;
  PUZZLE_SHARED = 0;
  PUZZLE_SOLVED = 0;

  unsigned long start_time, stop_time, total_time;

  bob_state_t state;
  bob_state_null(state);

  printf("Connecting to Tumbler...\n\n");
  void *context = zmq_ctx_new();
  void *socket = zmq_socket(context, ZMQ_REQ);
  int rc = zmq_connect(socket, TUMBLER_ENDPOINT);
  if (rc) {
    fprintf(stderr, "Error: could not connect to Tumbler.\n");
    exit(1);
  }

  TRY {
    bob_state_new(state);

    read_keys_from_file_alice_bob(BOB_KEY_FILE_PREFIX,
                                  state->keys->ec_sk,
                                  state->keys->ec_pk,
                                  state->keys->paillier_sk,
                                  state->keys->paillier_pk,
                                  state->tumbler_paillier_pk);

    start_time = timer();
    if (promise_init(socket) != RLC_OK) {
      THROW(ERR_CAUGHT);
    }

    while (!PROMISE_COMPLETED) {
      if (receive_message(state, socket) != RLC_OK) {
        THROW(ERR_CAUGHT);
      }
    }
    stop_time = timer();
    total_time = stop_time - start_time;
    printf("\nPromise procedure time: %.5f sec\n", total_time / CLOCK_PRECISION);

    rc = zmq_close(socket);
    if (rc) {
      fprintf(stderr, "Error: could not close the socket.\n");
      THROW(ERR_CAUGHT);
    }

    printf("Connecting to Alice...\n\n");
    socket = zmq_socket(context, ZMQ_REQ);
    rc = zmq_connect(socket, ALICE_ENDPOINT);
    if (rc) {
      fprintf(stderr, "Error: could not connect to Alice.\n");
      THROW(ERR_CAUGHT);
    }

    if (puzzle_share(state, socket) != RLC_OK) {
      THROW(ERR_CAUGHT);
    }

    while (!PUZZLE_SHARED) {
      if (receive_message(state, socket) != RLC_OK) {
        THROW(ERR_CAUGHT);
      }
    }

    rc = zmq_close(socket);
    if (rc) {
      fprintf(stderr, "Error: could not close the socket.\n");
      THROW(ERR_CAUGHT);
    }

    socket = zmq_socket(context, ZMQ_REP);
    rc = zmq_bind(socket, BOB_ENDPOINT);
    if (rc) {
      fprintf(stderr, "Error: could not bind the socket.\n");
      THROW(ERR_CAUGHT);
    }

    while (!PUZZLE_SOLVED) {
      if (receive_message(state, socket) != RLC_OK) {
        THROW(ERR_CAUGHT);
      }
    }
    stop_time = timer();
    total_time = stop_time - start_time;
    printf("\nTotal time: %.5f sec\n", total_time / CLOCK_PRECISION);
  } CATCH_ANY {
    result_status = RLC_ERR;
  } FINALLY {
    bob_state_free(state);
  }
  
  rc = zmq_close(socket);
  if (rc) {
    fprintf(stderr, "Error: could not close the socket.\n");
    THROW(ERR_CAUGHT);
  }
  zmq_ctx_destroy(context);
  clean();

  return result_status;
}