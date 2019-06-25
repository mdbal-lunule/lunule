/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2016 Intel Corporation. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *	 * Redistributions of source code must retain the above copyright
 *	   notice, this list of conditions and the following disclaimer.
 *	 * Redistributions in binary form must reproduce the above copyright
 *	   notice, this list of conditions and the following disclaimer in
 *	   the documentation and/or other materials provided with the
 *	   distribution.
 *	 * Neither the name of Intel Corporation nor the names of its
 *	   contributors may be used to endorse or promote products derived
 *	   from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef TEST_CRYPTODEV_BLOCKCIPHER_H_
#define TEST_CRYPTODEV_BLOCKCIPHER_H_

#ifndef BLOCKCIPHER_TEST_MSG_LEN
#define BLOCKCIPHER_TEST_MSG_LEN		256
#endif

#define BLOCKCIPHER_TEST_OP_ENCRYPT		0x01
#define BLOCKCIPHER_TEST_OP_DECRYPT		0x02
#define BLOCKCIPHER_TEST_OP_AUTH_GEN	0x04
#define BLOCKCIPHER_TEST_OP_AUTH_VERIFY	0x08

#define BLOCKCIPHER_TEST_FEATURE_OOP			0x01
#define BLOCKCIPHER_TEST_FEATURE_SESSIONLESS	0x02
#define BLOCKCIPHER_TEST_FEATURE_STOPPER	0x04 /* stop upon failing */

#define BLOCKCIPHER_TEST_TARGET_PMD_MB		0x0001 /* Multi-buffer flag */
#define BLOCKCIPHER_TEST_TARGET_PMD_QAT			0x0002 /* QAT flag */
#define BLOCKCIPHER_TEST_TARGET_PMD_OPENSSL	0x0004 /* SW OPENSSL flag */

#define BLOCKCIPHER_TEST_OP_CIPHER	(BLOCKCIPHER_TEST_OP_ENCRYPT | \
					BLOCKCIPHER_TEST_OP_DECRYPT)

#define BLOCKCIPHER_TEST_OP_AUTH	(BLOCKCIPHER_TEST_OP_AUTH_GEN | \
					BLOCKCIPHER_TEST_OP_AUTH_VERIFY)

#define BLOCKCIPHER_TEST_OP_ENC_AUTH_GEN	(BLOCKCIPHER_TEST_OP_ENCRYPT | \
					BLOCKCIPHER_TEST_OP_AUTH_GEN)

#define BLOCKCIPHER_TEST_OP_AUTH_VERIFY_DEC	(BLOCKCIPHER_TEST_OP_DECRYPT | \
					BLOCKCIPHER_TEST_OP_AUTH_VERIFY)

enum blockcipher_test_type {
	BLKCIPHER_AES_CHAIN_TYPE,	/* use aes_chain_test_cases[] */
	BLKCIPHER_AES_CIPHERONLY_TYPE,	/* use aes_cipheronly_test_cases[] */
	BLKCIPHER_3DES_CHAIN_TYPE,	/* use triple_des_chain_test_cases[] */
	BLKCIPHER_3DES_CIPHERONLY_TYPE,	/* triple_des_cipheronly_test_cases[] */
	BLKCIPHER_AUTHONLY_TYPE		/* use hash_test_cases[] */
};

struct blockcipher_test_case {
	const char *test_descr; /* test description */
	const struct blockcipher_test_data *test_data;
	uint8_t op_mask; /* operation mask */
	uint8_t feature_mask;
	uint32_t pmd_mask;
};

struct blockcipher_test_data {
	enum rte_crypto_cipher_algorithm crypto_algo;

	struct {
		uint8_t data[64];
		unsigned int len;
	} cipher_key;

	struct {
		uint8_t data[64] __rte_aligned(16);
		unsigned int len;
	} iv;

	struct {
		const uint8_t *data;
		unsigned int len;
	} plaintext;

	struct {
		const uint8_t *data;
		unsigned int len;
	} ciphertext;

	enum rte_crypto_auth_algorithm auth_algo;

	struct {
		uint8_t data[128];
		unsigned int len;
	} auth_key;

	struct {
		uint8_t data[128];
		unsigned int len;		/* for qat */
		unsigned int truncated_len;	/* for mb */
	} digest;
};

int
test_blockcipher_all_tests(struct rte_mempool *mbuf_pool,
	struct rte_mempool *op_mpool,
	uint8_t dev_id,
	enum rte_cryptodev_type cryptodev_type,
	enum blockcipher_test_type test_type);

#endif /* TEST_CRYPTODEV_BLOCKCIPHER_H_ */
