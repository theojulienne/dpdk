/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(C) 2021 Marvell.
 */

#ifndef __ROC_IE_H__
#define __ROC_IE_H__

enum {
	ROC_IE_SA_DIR_INBOUND = 0,
	ROC_IE_SA_DIR_OUTBOUND = 1,
};

enum {
	ROC_IE_SA_IP_VERSION_4 = 0,
	ROC_IE_SA_IP_VERSION_6 = 1,
};

enum {
	ROC_IE_SA_MODE_TRANSPORT = 0,
	ROC_IE_SA_MODE_TUNNEL = 1,
};

enum {
	ROC_IE_SA_PROTOCOL_AH = 0,
	ROC_IE_SA_PROTOCOL_ESP = 1,
};

enum {
	ROC_IE_SA_AES_KEY_LEN_128 = 1,
	ROC_IE_SA_AES_KEY_LEN_192 = 2,
	ROC_IE_SA_AES_KEY_LEN_256 = 3,
};

enum {
	ROC_IE_SA_ENC_NULL = 0,
	ROC_IE_SA_ENC_DES_CBC = 1,
	ROC_IE_SA_ENC_3DES_CBC = 2,
	ROC_IE_SA_ENC_AES_CBC = 3,
	ROC_IE_SA_ENC_AES_CTR = 4,
	ROC_IE_SA_ENC_AES_GCM = 5,
	ROC_IE_SA_ENC_AES_CCM = 6,
};

enum {
	ROC_IE_SA_AUTH_NULL = 0,
	ROC_IE_SA_AUTH_MD5 = 1,
	ROC_IE_SA_AUTH_SHA1 = 2,
	ROC_IE_SA_AUTH_SHA2_224 = 3,
	ROC_IE_SA_AUTH_SHA2_256 = 4,
	ROC_IE_SA_AUTH_SHA2_384 = 5,
	ROC_IE_SA_AUTH_SHA2_512 = 6,
	ROC_IE_SA_AUTH_AES_GMAC = 7,
	ROC_IE_SA_AUTH_AES_XCBC_128 = 8,
};

#endif /* __ROC_IE_H__ */
