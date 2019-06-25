/*
 * Copyright (c) 2016 QLogic Corporation.
 * All rights reserved.
 * www.qlogic.com
 *
 * See LICENSE.qede_pmd for copyright and licensing details.
 */

#include "bcm_osal.h"
#include "ecore.h"
#include "ecore_status.h"
#include "ecore_mcp.h"
#include "mcp_public.h"
#include "reg_addr.h"
#include "ecore_hw.h"
#include "ecore_init_fw_funcs.h"
#include "ecore_sriov.h"
#include "ecore_vf.h"
#include "ecore_iov_api.h"
#include "ecore_gtt_reg_addr.h"
#include "ecore_iro.h"
#include "ecore_dcbx.h"

#define CHIP_MCP_RESP_ITER_US 10
#define EMUL_MCP_RESP_ITER_US (1000 * 1000)

#define ECORE_DRV_MB_MAX_RETRIES (500 * 1000)	/* Account for 5 sec */
#define ECORE_MCP_RESET_RETRIES (50 * 1000)	/* Account for 500 msec */

#define DRV_INNER_WR(_p_hwfn, _p_ptt, _ptr, _offset, _val) \
	ecore_wr(_p_hwfn, _p_ptt, (_p_hwfn->mcp_info->_ptr + _offset), \
		 _val)

#define DRV_INNER_RD(_p_hwfn, _p_ptt, _ptr, _offset) \
	ecore_rd(_p_hwfn, _p_ptt, (_p_hwfn->mcp_info->_ptr + _offset))

#define DRV_MB_WR(_p_hwfn, _p_ptt, _field, _val) \
	DRV_INNER_WR(p_hwfn, _p_ptt, drv_mb_addr, \
		     OFFSETOF(struct public_drv_mb, _field), _val)

#define DRV_MB_RD(_p_hwfn, _p_ptt, _field) \
	DRV_INNER_RD(_p_hwfn, _p_ptt, drv_mb_addr, \
		     OFFSETOF(struct public_drv_mb, _field))

#define PDA_COMP (((FW_MAJOR_VERSION) + (FW_MINOR_VERSION << 8)) << \
	DRV_ID_PDA_COMP_VER_SHIFT)

#define MCP_BYTES_PER_MBIT_SHIFT 17

#ifndef ASIC_ONLY
static int loaded;
static int loaded_port[MAX_NUM_PORTS] = { 0 };
#endif

bool ecore_mcp_is_init(struct ecore_hwfn *p_hwfn)
{
	if (!p_hwfn->mcp_info || !p_hwfn->mcp_info->public_base)
		return false;
	return true;
}

void ecore_mcp_cmd_port_init(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt)
{
	u32 addr = SECTION_OFFSIZE_ADDR(p_hwfn->mcp_info->public_base,
					PUBLIC_PORT);
	u32 mfw_mb_offsize = ecore_rd(p_hwfn, p_ptt, addr);

	p_hwfn->mcp_info->port_addr = SECTION_ADDR(mfw_mb_offsize,
						   MFW_PORT(p_hwfn));
	DP_VERBOSE(p_hwfn, ECORE_MSG_SP,
		   "port_addr = 0x%x, port_id 0x%02x\n",
		   p_hwfn->mcp_info->port_addr, MFW_PORT(p_hwfn));
}

void ecore_mcp_read_mb(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt)
{
	u32 length = MFW_DRV_MSG_MAX_DWORDS(p_hwfn->mcp_info->mfw_mb_length);
	OSAL_BE32 tmp;
	u32 i;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_TEDIBEAR(p_hwfn->p_dev))
		return;
#endif

	if (!p_hwfn->mcp_info->public_base)
		return;

	for (i = 0; i < length; i++) {
		tmp = ecore_rd(p_hwfn, p_ptt,
			       p_hwfn->mcp_info->mfw_mb_addr +
			       (i << 2) + sizeof(u32));

		((u32 *)p_hwfn->mcp_info->mfw_mb_cur)[i] =
		    OSAL_BE32_TO_CPU(tmp);
	}
}

enum _ecore_status_t ecore_mcp_free(struct ecore_hwfn *p_hwfn)
{
	if (p_hwfn->mcp_info) {
		OSAL_FREE(p_hwfn->p_dev, p_hwfn->mcp_info->mfw_mb_cur);
		OSAL_FREE(p_hwfn->p_dev, p_hwfn->mcp_info->mfw_mb_shadow);
		OSAL_SPIN_LOCK_DEALLOC(&p_hwfn->mcp_info->lock);
	}
	OSAL_FREE(p_hwfn->p_dev, p_hwfn->mcp_info);
	p_hwfn->mcp_info = OSAL_NULL;

	return ECORE_SUCCESS;
}

static enum _ecore_status_t ecore_load_mcp_offsets(struct ecore_hwfn *p_hwfn,
						   struct ecore_ptt *p_ptt)
{
	struct ecore_mcp_info *p_info = p_hwfn->mcp_info;
	u32 drv_mb_offsize, mfw_mb_offsize;
	u32 mcp_pf_id = MCP_PF_ID(p_hwfn);

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_EMUL(p_hwfn->p_dev)) {
		DP_NOTICE(p_hwfn, false, "Emulation - assume no MFW\n");
		p_info->public_base = 0;
		return ECORE_INVAL;
	}
#endif

	p_info->public_base = ecore_rd(p_hwfn, p_ptt, MISC_REG_SHARED_MEM_ADDR);
	if (!p_info->public_base)
		return ECORE_INVAL;

	p_info->public_base |= GRCBASE_MCP;

	/* Calculate the driver and MFW mailbox address */
	drv_mb_offsize = ecore_rd(p_hwfn, p_ptt,
				  SECTION_OFFSIZE_ADDR(p_info->public_base,
						       PUBLIC_DRV_MB));
	p_info->drv_mb_addr = SECTION_ADDR(drv_mb_offsize, mcp_pf_id);
	DP_VERBOSE(p_hwfn, ECORE_MSG_SP,
		   "drv_mb_offsiz = 0x%x, drv_mb_addr = 0x%x"
		   " mcp_pf_id = 0x%x\n",
		   drv_mb_offsize, p_info->drv_mb_addr, mcp_pf_id);

	/* Set the MFW MB address */
	mfw_mb_offsize = ecore_rd(p_hwfn, p_ptt,
				  SECTION_OFFSIZE_ADDR(p_info->public_base,
						       PUBLIC_MFW_MB));
	p_info->mfw_mb_addr = SECTION_ADDR(mfw_mb_offsize, mcp_pf_id);
	p_info->mfw_mb_length = (u16)ecore_rd(p_hwfn, p_ptt,
					       p_info->mfw_mb_addr);

	/* Get the current driver mailbox sequence before sending
	 * the first command
	 */
	p_info->drv_mb_seq = DRV_MB_RD(p_hwfn, p_ptt, drv_mb_header) &
	    DRV_MSG_SEQ_NUMBER_MASK;

	/* Get current FW pulse sequence */
	p_info->drv_pulse_seq = DRV_MB_RD(p_hwfn, p_ptt, drv_pulse_mb) &
	    DRV_PULSE_SEQ_MASK;

	p_info->mcp_hist = (u16)ecore_rd(p_hwfn, p_ptt,
					  MISCS_REG_GENERIC_POR_0);

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_cmd_init(struct ecore_hwfn *p_hwfn,
					struct ecore_ptt *p_ptt)
{
	struct ecore_mcp_info *p_info;
	u32 size;

	/* Allocate mcp_info structure */
	p_hwfn->mcp_info = OSAL_ZALLOC(p_hwfn->p_dev, GFP_KERNEL,
				       sizeof(*p_hwfn->mcp_info));
	if (!p_hwfn->mcp_info)
		goto err;
	p_info = p_hwfn->mcp_info;

	if (ecore_load_mcp_offsets(p_hwfn, p_ptt) != ECORE_SUCCESS) {
		DP_NOTICE(p_hwfn, false, "MCP is not initialized\n");
		/* Do not free mcp_info here, since public_base indicate that
		 * the MCP is not initialized
		 */
		return ECORE_SUCCESS;
	}

	size = MFW_DRV_MSG_MAX_DWORDS(p_info->mfw_mb_length) * sizeof(u32);
	p_info->mfw_mb_cur = OSAL_ZALLOC(p_hwfn->p_dev, GFP_KERNEL, size);
	p_info->mfw_mb_shadow = OSAL_ZALLOC(p_hwfn->p_dev, GFP_KERNEL, size);
	if (!p_info->mfw_mb_shadow || !p_info->mfw_mb_addr)
		goto err;

	/* Initialize the MFW spinlock */
	OSAL_SPIN_LOCK_ALLOC(p_hwfn, &p_info->lock);
	OSAL_SPIN_LOCK_INIT(&p_info->lock);

	return ECORE_SUCCESS;

err:
	DP_NOTICE(p_hwfn, true, "Failed to allocate mcp memory\n");
	ecore_mcp_free(p_hwfn);
	return ECORE_NOMEM;
}

/* Locks the MFW mailbox of a PF to ensure a single access.
 * The lock is achieved in most cases by holding a spinlock, causing other
 * threads to wait till a previous access is done.
 * In some cases (currently when a [UN]LOAD_REQ commands are sent), the single
 * access is achieved by setting a blocking flag, which will fail other
 * competing contexts to send their mailboxes.
 */
static enum _ecore_status_t ecore_mcp_mb_lock(struct ecore_hwfn *p_hwfn,
					      u32 cmd)
{
	OSAL_SPIN_LOCK(&p_hwfn->mcp_info->lock);

	/* The spinlock shouldn't be acquired when the mailbox command is
	 * [UN]LOAD_REQ, since the engine is locked by the MFW, and a parallel
	 * pending [UN]LOAD_REQ command of another PF together with a spinlock
	 * (i.e. interrupts are disabled) - can lead to a deadlock.
	 * It is assumed that for a single PF, no other mailbox commands can be
	 * sent from another context while sending LOAD_REQ, and that any
	 * parallel commands to UNLOAD_REQ can be cancelled.
	 */
	if (cmd == DRV_MSG_CODE_LOAD_DONE || cmd == DRV_MSG_CODE_UNLOAD_DONE)
		p_hwfn->mcp_info->block_mb_sending = false;

	if (p_hwfn->mcp_info->block_mb_sending) {
		DP_NOTICE(p_hwfn, false,
			  "Trying to send a MFW mailbox command [0x%x]"
			  " in parallel to [UN]LOAD_REQ. Aborting.\n",
			  cmd);
		OSAL_SPIN_UNLOCK(&p_hwfn->mcp_info->lock);
		return ECORE_BUSY;
	}

	if (cmd == DRV_MSG_CODE_LOAD_REQ || cmd == DRV_MSG_CODE_UNLOAD_REQ) {
		p_hwfn->mcp_info->block_mb_sending = true;
		OSAL_SPIN_UNLOCK(&p_hwfn->mcp_info->lock);
	}

	return ECORE_SUCCESS;
}

static void ecore_mcp_mb_unlock(struct ecore_hwfn *p_hwfn, u32 cmd)
{
	if (cmd != DRV_MSG_CODE_LOAD_REQ && cmd != DRV_MSG_CODE_UNLOAD_REQ)
		OSAL_SPIN_UNLOCK(&p_hwfn->mcp_info->lock);
}

enum _ecore_status_t ecore_mcp_reset(struct ecore_hwfn *p_hwfn,
				     struct ecore_ptt *p_ptt)
{
	u32 seq = ++p_hwfn->mcp_info->drv_mb_seq;
	u32 delay = CHIP_MCP_RESP_ITER_US;
	u32 org_mcp_reset_seq, cnt = 0;
	enum _ecore_status_t rc = ECORE_SUCCESS;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_EMUL(p_hwfn->p_dev))
		delay = EMUL_MCP_RESP_ITER_US;
#endif

	/* Ensure that only a single thread is accessing the mailbox at a
	 * certain time.
	 */
	rc = ecore_mcp_mb_lock(p_hwfn, DRV_MSG_CODE_MCP_RESET);
	if (rc != ECORE_SUCCESS)
		return rc;

	/* Set drv command along with the updated sequence */
	org_mcp_reset_seq = ecore_rd(p_hwfn, p_ptt, MISCS_REG_GENERIC_POR_0);
	DRV_MB_WR(p_hwfn, p_ptt, drv_mb_header, (DRV_MSG_CODE_MCP_RESET | seq));

	do {
		/* Wait for MFW response */
		OSAL_UDELAY(delay);
		/* Give the FW up to 500 second (50*1000*10usec) */
	} while ((org_mcp_reset_seq == ecore_rd(p_hwfn, p_ptt,
						MISCS_REG_GENERIC_POR_0)) &&
		 (cnt++ < ECORE_MCP_RESET_RETRIES));

	if (org_mcp_reset_seq !=
	    ecore_rd(p_hwfn, p_ptt, MISCS_REG_GENERIC_POR_0)) {
		DP_VERBOSE(p_hwfn, ECORE_MSG_SP,
			   "MCP was reset after %d usec\n", cnt * delay);
	} else {
		DP_ERR(p_hwfn, "Failed to reset MCP\n");
		rc = ECORE_AGAIN;
	}

	ecore_mcp_mb_unlock(p_hwfn, DRV_MSG_CODE_MCP_RESET);

	return rc;
}

static enum _ecore_status_t ecore_do_mcp_cmd(struct ecore_hwfn *p_hwfn,
					     struct ecore_ptt *p_ptt,
					     u32 cmd, u32 param,
					     u32 *o_mcp_resp,
					     u32 *o_mcp_param)
{
	u32 delay = CHIP_MCP_RESP_ITER_US;
	u32 max_retries = ECORE_DRV_MB_MAX_RETRIES;
	u32 seq, cnt = 1, actual_mb_seq;
	enum _ecore_status_t rc = ECORE_SUCCESS;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_EMUL(p_hwfn->p_dev))
		delay = EMUL_MCP_RESP_ITER_US;
	/* There is a built-in delay of 100usec in each MFW response read */
	if (CHIP_REV_IS_FPGA(p_hwfn->p_dev))
		max_retries /= 10;
#endif

	/* Get actual driver mailbox sequence */
	actual_mb_seq = DRV_MB_RD(p_hwfn, p_ptt, drv_mb_header) &
	    DRV_MSG_SEQ_NUMBER_MASK;

	/* Use MCP history register to check if MCP reset occurred between
	 * init time and now.
	 */
	if (p_hwfn->mcp_info->mcp_hist !=
	    ecore_rd(p_hwfn, p_ptt, MISCS_REG_GENERIC_POR_0)) {
		DP_VERBOSE(p_hwfn, ECORE_MSG_SP, "Rereading MCP offsets\n");
		ecore_load_mcp_offsets(p_hwfn, p_ptt);
		ecore_mcp_cmd_port_init(p_hwfn, p_ptt);
	}
	seq = ++p_hwfn->mcp_info->drv_mb_seq;

	/* Set drv param */
	DRV_MB_WR(p_hwfn, p_ptt, drv_mb_param, param);

	/* Set drv command along with the updated sequence */
	DRV_MB_WR(p_hwfn, p_ptt, drv_mb_header, (cmd | seq));

	do {
		/* Wait for MFW response */
		OSAL_UDELAY(delay);
		*o_mcp_resp = DRV_MB_RD(p_hwfn, p_ptt, fw_mb_header);

		/* Give the FW up to 5 second (500*10ms) */
	} while ((seq != (*o_mcp_resp & FW_MSG_SEQ_NUMBER_MASK)) &&
		 (cnt++ < max_retries));

	/* Is this a reply to our command? */
	if (seq == (*o_mcp_resp & FW_MSG_SEQ_NUMBER_MASK)) {
		*o_mcp_resp &= FW_MSG_CODE_MASK;
		/* Get the MCP param */
		*o_mcp_param = DRV_MB_RD(p_hwfn, p_ptt, fw_mb_param);
	} else {
		/* FW BUG! */
		DP_ERR(p_hwfn, "MFW failed to respond [cmd 0x%x param 0x%x]\n",
		       cmd, param);
		*o_mcp_resp = 0;
		rc = ECORE_AGAIN;
		ecore_hw_err_notify(p_hwfn, ECORE_HW_ERR_MFW_RESP_FAIL);
	}
	return rc;
}

static enum _ecore_status_t
ecore_mcp_cmd_and_union(struct ecore_hwfn *p_hwfn,
			struct ecore_ptt *p_ptt,
			struct ecore_mcp_mb_params *p_mb_params)
{
	u32 union_data_addr;
	enum _ecore_status_t rc;

	/* MCP not initialized */
	if (!ecore_mcp_is_init(p_hwfn)) {
		DP_NOTICE(p_hwfn, true, "MFW is not initialized !\n");
		return ECORE_BUSY;
	}

	union_data_addr = p_hwfn->mcp_info->drv_mb_addr +
			  OFFSETOF(struct public_drv_mb, union_data);

	/* Ensure that only a single thread is accessing the mailbox at a
	 * certain time.
	 */
	rc = ecore_mcp_mb_lock(p_hwfn, p_mb_params->cmd);
	if (rc != ECORE_SUCCESS)
		return rc;

	if (p_mb_params->p_data_src != OSAL_NULL)
		ecore_memcpy_to(p_hwfn, p_ptt, union_data_addr,
				p_mb_params->p_data_src,
				sizeof(*p_mb_params->p_data_src));

	rc = ecore_do_mcp_cmd(p_hwfn, p_ptt, p_mb_params->cmd,
			      p_mb_params->param, &p_mb_params->mcp_resp,
			      &p_mb_params->mcp_param);

	if (p_mb_params->p_data_dst != OSAL_NULL)
		ecore_memcpy_from(p_hwfn, p_ptt, p_mb_params->p_data_dst,
				  union_data_addr,
				  sizeof(*p_mb_params->p_data_dst));

	ecore_mcp_mb_unlock(p_hwfn, p_mb_params->cmd);

	return rc;
}

enum _ecore_status_t ecore_mcp_cmd(struct ecore_hwfn *p_hwfn,
				   struct ecore_ptt *p_ptt, u32 cmd, u32 param,
				   u32 *o_mcp_resp, u32 *o_mcp_param)
{
	struct ecore_mcp_mb_params mb_params;
	enum _ecore_status_t rc;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_EMUL(p_hwfn->p_dev)) {
		if (cmd == DRV_MSG_CODE_UNLOAD_REQ) {
			loaded--;
			loaded_port[p_hwfn->port_id]--;
			DP_VERBOSE(p_hwfn, ECORE_MSG_SP, "Unload cnt: 0x%x\n",
				   loaded);
		}
		return ECORE_SUCCESS;
	}
#endif

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = cmd;
	mb_params.param = param;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);
	if (rc != ECORE_SUCCESS)
		return rc;

	*o_mcp_resp = mb_params.mcp_resp;
	*o_mcp_param = mb_params.mcp_param;

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_nvm_wr_cmd(struct ecore_hwfn *p_hwfn,
					  struct ecore_ptt *p_ptt,
					  u32 cmd,
					  u32 param,
					  u32 *o_mcp_resp,
					  u32 *o_mcp_param,
					  u32 i_txn_size, u32 *i_buf)
{
	struct ecore_mcp_mb_params mb_params;
	union drv_union_data union_data;
	enum _ecore_status_t rc;

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = cmd;
	mb_params.param = param;
	OSAL_MEMCPY((u32 *)&union_data.raw_data, i_buf, i_txn_size);
	mb_params.p_data_src = &union_data;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);
	if (rc != ECORE_SUCCESS)
		return rc;

	*o_mcp_resp = mb_params.mcp_resp;
	*o_mcp_param = mb_params.mcp_param;

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_nvm_rd_cmd(struct ecore_hwfn *p_hwfn,
					  struct ecore_ptt *p_ptt,
					  u32 cmd,
					  u32 param,
					  u32 *o_mcp_resp,
					  u32 *o_mcp_param,
					  u32 *o_txn_size, u32 *o_buf)
{
	struct ecore_mcp_mb_params mb_params;
	union drv_union_data union_data;
	enum _ecore_status_t rc;

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = cmd;
	mb_params.param = param;
	mb_params.p_data_dst = &union_data;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);
	if (rc != ECORE_SUCCESS)
		return rc;

	*o_mcp_resp = mb_params.mcp_resp;
	*o_mcp_param = mb_params.mcp_param;

	*o_txn_size = *o_mcp_param;
	OSAL_MEMCPY(o_buf, (u32 *)&union_data.raw_data, *o_txn_size);

	return ECORE_SUCCESS;
}

#ifndef ASIC_ONLY
static void ecore_mcp_mf_workaround(struct ecore_hwfn *p_hwfn,
				    u32 *p_load_code)
{
	static int load_phase = FW_MSG_CODE_DRV_LOAD_ENGINE;

	if (!loaded)
		load_phase = FW_MSG_CODE_DRV_LOAD_ENGINE;
	else if (!loaded_port[p_hwfn->port_id])
		load_phase = FW_MSG_CODE_DRV_LOAD_PORT;
	else
		load_phase = FW_MSG_CODE_DRV_LOAD_FUNCTION;

	/* On CMT, always tell that it's engine */
	if (p_hwfn->p_dev->num_hwfns > 1)
		load_phase = FW_MSG_CODE_DRV_LOAD_ENGINE;

	*p_load_code = load_phase;
	loaded++;
	loaded_port[p_hwfn->port_id]++;

	DP_VERBOSE(p_hwfn, ECORE_MSG_SP,
		   "Load phase: %x load cnt: 0x%x port id=%d port_load=%d\n",
		   *p_load_code, loaded, p_hwfn->port_id,
		   loaded_port[p_hwfn->port_id]);
}
#endif

enum _ecore_status_t ecore_mcp_load_req(struct ecore_hwfn *p_hwfn,
					struct ecore_ptt *p_ptt,
					u32 *p_load_code)
{
	struct ecore_dev *p_dev = p_hwfn->p_dev;
	struct ecore_mcp_mb_params mb_params;
	union drv_union_data union_data;
	enum _ecore_status_t rc;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_EMUL(p_hwfn->p_dev)) {
		ecore_mcp_mf_workaround(p_hwfn, p_load_code);
		return ECORE_SUCCESS;
	}
#endif

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = DRV_MSG_CODE_LOAD_REQ;
	mb_params.param = PDA_COMP | DRV_ID_MCP_HSI_VER_CURRENT |
			  p_dev->drv_type;
	OSAL_MEMCPY(&union_data.ver_str, p_dev->ver_str, MCP_DRV_VER_STR_SIZE);
	mb_params.p_data_src = &union_data;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);

	/* if mcp fails to respond we must abort */
	if (rc != ECORE_SUCCESS) {
		DP_ERR(p_hwfn, "MCP response failure, aborting\n");
		return rc;
	}

	*p_load_code = mb_params.mcp_resp;

	/* If MFW refused (e.g. other port is in diagnostic mode) we
	 * must abort. This can happen in the following cases:
	 * - Other port is in diagnostic mode
	 * - Previously loaded function on the engine is not compliant with
	 *   the requester.
	 * - MFW cannot cope with the requester's DRV_MFW_HSI_VERSION.
	 *      -
	 */
	if (!(*p_load_code) ||
	    ((*p_load_code) == FW_MSG_CODE_DRV_LOAD_REFUSED_HSI) ||
	    ((*p_load_code) == FW_MSG_CODE_DRV_LOAD_REFUSED_PDA) ||
	    ((*p_load_code) == FW_MSG_CODE_DRV_LOAD_REFUSED_DIAG)) {
		DP_ERR(p_hwfn, "MCP refused load request, aborting\n");
		return ECORE_BUSY;
	}

	return ECORE_SUCCESS;
}

static void ecore_mcp_handle_vf_flr(struct ecore_hwfn *p_hwfn,
				    struct ecore_ptt *p_ptt)
{
	u32 addr = SECTION_OFFSIZE_ADDR(p_hwfn->mcp_info->public_base,
					PUBLIC_PATH);
	u32 mfw_path_offsize = ecore_rd(p_hwfn, p_ptt, addr);
	u32 path_addr = SECTION_ADDR(mfw_path_offsize,
				     ECORE_PATH_ID(p_hwfn));
	u32 disabled_vfs[VF_MAX_STATIC / 32];
	int i;

	DP_VERBOSE(p_hwfn, ECORE_MSG_SP,
		   "Reading Disabled VF information from [offset %08x],"
		   " path_addr %08x\n",
		   mfw_path_offsize, path_addr);

	for (i = 0; i < (VF_MAX_STATIC / 32); i++) {
		disabled_vfs[i] = ecore_rd(p_hwfn, p_ptt,
					   path_addr +
					   OFFSETOF(struct public_path,
						    mcp_vf_disabled) +
					   sizeof(u32) * i);
		DP_VERBOSE(p_hwfn, (ECORE_MSG_SP | ECORE_MSG_IOV),
			   "FLR-ed VFs [%08x,...,%08x] - %08x\n",
			   i * 32, (i + 1) * 32 - 1, disabled_vfs[i]);
	}

	if (ecore_iov_mark_vf_flr(p_hwfn, disabled_vfs))
		OSAL_VF_FLR_UPDATE(p_hwfn);
}

enum _ecore_status_t ecore_mcp_ack_vf_flr(struct ecore_hwfn *p_hwfn,
					  struct ecore_ptt *p_ptt,
					  u32 *vfs_to_ack)
{
	u32 addr = SECTION_OFFSIZE_ADDR(p_hwfn->mcp_info->public_base,
					PUBLIC_FUNC);
	u32 mfw_func_offsize = ecore_rd(p_hwfn, p_ptt, addr);
	u32 func_addr = SECTION_ADDR(mfw_func_offsize,
				     MCP_PF_ID(p_hwfn));
	struct ecore_mcp_mb_params mb_params;
	union drv_union_data union_data;
	enum _ecore_status_t rc;
	int i;

	for (i = 0; i < (VF_MAX_STATIC / 32); i++)
		DP_VERBOSE(p_hwfn, (ECORE_MSG_SP | ECORE_MSG_IOV),
			   "Acking VFs [%08x,...,%08x] - %08x\n",
			   i * 32, (i + 1) * 32 - 1, vfs_to_ack[i]);

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = DRV_MSG_CODE_VF_DISABLED_DONE;
	OSAL_MEMCPY(&union_data.ack_vf_disabled, vfs_to_ack, VF_MAX_STATIC / 8);
	mb_params.p_data_src = &union_data;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt,
				     &mb_params);
	if (rc != ECORE_SUCCESS) {
		DP_NOTICE(p_hwfn, false,
			  "Failed to pass ACK for VF flr to MFW\n");
		return ECORE_TIMEOUT;
	}

	/* TMP - clear the ACK bits; should be done by MFW */
	for (i = 0; i < (VF_MAX_STATIC / 32); i++)
		ecore_wr(p_hwfn, p_ptt,
			 func_addr +
			 OFFSETOF(struct public_func, drv_ack_vf_disabled) +
			 i * sizeof(u32), 0);

	return rc;
}

static void ecore_mcp_handle_transceiver_change(struct ecore_hwfn *p_hwfn,
						struct ecore_ptt *p_ptt)
{
	u32 transceiver_state;

	transceiver_state = ecore_rd(p_hwfn, p_ptt,
				     p_hwfn->mcp_info->port_addr +
				     OFFSETOF(struct public_port,
					      transceiver_data));

	DP_VERBOSE(p_hwfn, (ECORE_MSG_HW | ECORE_MSG_SP),
		   "Received transceiver state update [0x%08x] from mfw"
		   " [Addr 0x%x]\n",
		   transceiver_state, (u32)(p_hwfn->mcp_info->port_addr +
					    OFFSETOF(struct public_port,
						     transceiver_data)));

	transceiver_state = GET_FIELD(transceiver_state, ETH_TRANSCEIVER_STATE);

	if (transceiver_state == ETH_TRANSCEIVER_STATE_PRESENT)
		DP_NOTICE(p_hwfn, false, "Transceiver is present.\n");
	else
		DP_NOTICE(p_hwfn, false, "Transceiver is unplugged.\n");
}

static void ecore_mcp_handle_link_change(struct ecore_hwfn *p_hwfn,
					 struct ecore_ptt *p_ptt,
					 bool b_reset)
{
	struct ecore_mcp_link_state *p_link;
	u8 max_bw, min_bw;
	u32 status = 0;

	p_link = &p_hwfn->mcp_info->link_output;
	OSAL_MEMSET(p_link, 0, sizeof(*p_link));
	if (!b_reset) {
		status = ecore_rd(p_hwfn, p_ptt,
				  p_hwfn->mcp_info->port_addr +
				  OFFSETOF(struct public_port, link_status));
		DP_VERBOSE(p_hwfn, (ECORE_MSG_LINK | ECORE_MSG_SP),
			   "Received link update [0x%08x] from mfw"
			   " [Addr 0x%x]\n",
			   status, (u32)(p_hwfn->mcp_info->port_addr +
					  OFFSETOF(struct public_port,
						   link_status)));
	} else {
		DP_VERBOSE(p_hwfn, ECORE_MSG_LINK,
			   "Resetting link indications\n");
		return;
	}

	if (p_hwfn->b_drv_link_init)
		p_link->link_up = !!(status & LINK_STATUS_LINK_UP);
	else
		p_link->link_up = false;

	p_link->full_duplex = true;
	switch ((status & LINK_STATUS_SPEED_AND_DUPLEX_MASK)) {
	case LINK_STATUS_SPEED_AND_DUPLEX_100G:
		p_link->speed = 100000;
		break;
	case LINK_STATUS_SPEED_AND_DUPLEX_50G:
		p_link->speed = 50000;
		break;
	case LINK_STATUS_SPEED_AND_DUPLEX_40G:
		p_link->speed = 40000;
		break;
	case LINK_STATUS_SPEED_AND_DUPLEX_25G:
		p_link->speed = 25000;
		break;
	case LINK_STATUS_SPEED_AND_DUPLEX_20G:
		p_link->speed = 20000;
		break;
	case LINK_STATUS_SPEED_AND_DUPLEX_10G:
		p_link->speed = 10000;
		break;
	case LINK_STATUS_SPEED_AND_DUPLEX_1000THD:
		p_link->full_duplex = false;
		/* Fall-through */
	case LINK_STATUS_SPEED_AND_DUPLEX_1000TFD:
		p_link->speed = 1000;
		break;
	default:
		p_link->speed = 0;
	}

	/* We never store total line speed as p_link->speed is
	 * again changes according to bandwidth allocation.
	 */
	if (p_link->link_up && p_link->speed)
		p_link->line_speed = p_link->speed;
	else
		p_link->line_speed = 0;

	max_bw = p_hwfn->mcp_info->func_info.bandwidth_max;
	min_bw = p_hwfn->mcp_info->func_info.bandwidth_min;

	/* Max bandwidth configuration */
	__ecore_configure_pf_max_bandwidth(p_hwfn, p_ptt,
					   p_link, max_bw);

	/* Mintz bandwidth configuration */
	__ecore_configure_pf_min_bandwidth(p_hwfn, p_ptt,
					   p_link, min_bw);
	ecore_configure_vp_wfq_on_link_change(p_hwfn->p_dev,
					      p_link->min_pf_rate);

	p_link->an = !!(status & LINK_STATUS_AUTO_NEGOTIATE_ENABLED);
	p_link->an_complete = !!(status & LINK_STATUS_AUTO_NEGOTIATE_COMPLETE);
	p_link->parallel_detection = !!(status &
					 LINK_STATUS_PARALLEL_DETECTION_USED);
	p_link->pfc_enabled = !!(status & LINK_STATUS_PFC_ENABLED);

	p_link->partner_adv_speed |=
	    (status & LINK_STATUS_LINK_PARTNER_1000TFD_CAPABLE) ?
	    ECORE_LINK_PARTNER_SPEED_1G_FD : 0;
	p_link->partner_adv_speed |=
	    (status & LINK_STATUS_LINK_PARTNER_1000THD_CAPABLE) ?
	    ECORE_LINK_PARTNER_SPEED_1G_HD : 0;
	p_link->partner_adv_speed |=
	    (status & LINK_STATUS_LINK_PARTNER_10G_CAPABLE) ?
	    ECORE_LINK_PARTNER_SPEED_10G : 0;
	p_link->partner_adv_speed |=
	    (status & LINK_STATUS_LINK_PARTNER_20G_CAPABLE) ?
	    ECORE_LINK_PARTNER_SPEED_20G : 0;
	p_link->partner_adv_speed |=
	    (status & LINK_STATUS_LINK_PARTNER_25G_CAPABLE) ?
	    ECORE_LINK_PARTNER_SPEED_25G : 0;
	p_link->partner_adv_speed |=
	    (status & LINK_STATUS_LINK_PARTNER_40G_CAPABLE) ?
	    ECORE_LINK_PARTNER_SPEED_40G : 0;
	p_link->partner_adv_speed |=
	    (status & LINK_STATUS_LINK_PARTNER_50G_CAPABLE) ?
	    ECORE_LINK_PARTNER_SPEED_50G : 0;
	p_link->partner_adv_speed |=
	    (status & LINK_STATUS_LINK_PARTNER_100G_CAPABLE) ?
	    ECORE_LINK_PARTNER_SPEED_100G : 0;

	p_link->partner_tx_flow_ctrl_en =
	    !!(status & LINK_STATUS_TX_FLOW_CONTROL_ENABLED);
	p_link->partner_rx_flow_ctrl_en =
	    !!(status & LINK_STATUS_RX_FLOW_CONTROL_ENABLED);

	switch (status & LINK_STATUS_LINK_PARTNER_FLOW_CONTROL_MASK) {
	case LINK_STATUS_LINK_PARTNER_SYMMETRIC_PAUSE:
		p_link->partner_adv_pause = ECORE_LINK_PARTNER_SYMMETRIC_PAUSE;
		break;
	case LINK_STATUS_LINK_PARTNER_ASYMMETRIC_PAUSE:
		p_link->partner_adv_pause = ECORE_LINK_PARTNER_ASYMMETRIC_PAUSE;
		break;
	case LINK_STATUS_LINK_PARTNER_BOTH_PAUSE:
		p_link->partner_adv_pause = ECORE_LINK_PARTNER_BOTH_PAUSE;
		break;
	default:
		p_link->partner_adv_pause = 0;
	}

	p_link->sfp_tx_fault = !!(status & LINK_STATUS_SFP_TX_FAULT);

	if (p_link->link_up)
		ecore_dcbx_eagle_workaround(p_hwfn, p_ptt, p_link->pfc_enabled);

	OSAL_LINK_UPDATE(p_hwfn);
}

enum _ecore_status_t ecore_mcp_set_link(struct ecore_hwfn *p_hwfn,
					struct ecore_ptt *p_ptt, bool b_up)
{
	struct ecore_mcp_link_params *params = &p_hwfn->mcp_info->link_input;
	struct ecore_mcp_mb_params mb_params;
	union drv_union_data union_data;
	struct eth_phy_cfg *p_phy_cfg;
	enum _ecore_status_t rc = ECORE_SUCCESS;
	u32 cmd;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_EMUL(p_hwfn->p_dev))
		return ECORE_SUCCESS;
#endif

	/* Set the shmem configuration according to params */
	p_phy_cfg = &union_data.drv_phy_cfg;
	OSAL_MEMSET(p_phy_cfg, 0, sizeof(*p_phy_cfg));
	cmd = b_up ? DRV_MSG_CODE_INIT_PHY : DRV_MSG_CODE_LINK_RESET;
	if (!params->speed.autoneg)
		p_phy_cfg->speed = params->speed.forced_speed;
	p_phy_cfg->pause |= (params->pause.autoneg) ? ETH_PAUSE_AUTONEG : 0;
	p_phy_cfg->pause |= (params->pause.forced_rx) ? ETH_PAUSE_RX : 0;
	p_phy_cfg->pause |= (params->pause.forced_tx) ? ETH_PAUSE_TX : 0;
	p_phy_cfg->adv_speed = params->speed.advertised_speeds;
	p_phy_cfg->loopback_mode = params->loopback_mode;
	p_hwfn->b_drv_link_init = b_up;

	if (b_up)
		DP_VERBOSE(p_hwfn, ECORE_MSG_LINK,
			   "Configuring Link: Speed 0x%08x, Pause 0x%08x,"
			   " adv_speed 0x%08x, loopback 0x%08x,"
			   " features 0x%08x\n",
			   p_phy_cfg->speed, p_phy_cfg->pause,
			   p_phy_cfg->adv_speed, p_phy_cfg->loopback_mode,
			   p_phy_cfg->feature_config_flags);
	else
		DP_VERBOSE(p_hwfn, ECORE_MSG_LINK, "Resetting link\n");

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = cmd;
	mb_params.p_data_src = &union_data;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);

	/* if mcp fails to respond we must abort */
	if (rc != ECORE_SUCCESS) {
		DP_ERR(p_hwfn, "MCP response failure, aborting\n");
		return rc;
	}

	/* Reset the link status if needed */
	if (!b_up)
		ecore_mcp_handle_link_change(p_hwfn, p_ptt, true);

	return rc;
}

u32 ecore_get_process_kill_counter(struct ecore_hwfn *p_hwfn,
				   struct ecore_ptt *p_ptt)
{
	u32 path_offsize_addr, path_offsize, path_addr, proc_kill_cnt;

	/* TODO - Add support for VFs */
	if (IS_VF(p_hwfn->p_dev))
		return ECORE_INVAL;

	path_offsize_addr = SECTION_OFFSIZE_ADDR(p_hwfn->mcp_info->public_base,
						 PUBLIC_PATH);
	path_offsize = ecore_rd(p_hwfn, p_ptt, path_offsize_addr);
	path_addr = SECTION_ADDR(path_offsize, ECORE_PATH_ID(p_hwfn));

	proc_kill_cnt = ecore_rd(p_hwfn, p_ptt,
				 path_addr +
				 OFFSETOF(struct public_path, process_kill)) &
	    PROCESS_KILL_COUNTER_MASK;

	return proc_kill_cnt;
}

static void ecore_mcp_handle_process_kill(struct ecore_hwfn *p_hwfn,
					  struct ecore_ptt *p_ptt)
{
	struct ecore_dev *p_dev = p_hwfn->p_dev;
	u32 proc_kill_cnt;

	/* Prevent possible attentions/interrupts during the recovery handling
	 * and till its load phase, during which they will be re-enabled.
	 */
	ecore_int_igu_disable_int(p_hwfn, p_ptt);

	DP_NOTICE(p_hwfn, false, "Received a process kill indication\n");

	/* The following operations should be done once, and thus in CMT mode
	 * are carried out by only the first HW function.
	 */
	if (p_hwfn != ECORE_LEADING_HWFN(p_dev))
		return;

	if (p_dev->recov_in_prog) {
		DP_NOTICE(p_hwfn, false,
			  "Ignoring the indication since a recovery"
			  " process is already in progress\n");
		return;
	}

	p_dev->recov_in_prog = true;

	proc_kill_cnt = ecore_get_process_kill_counter(p_hwfn, p_ptt);
	DP_NOTICE(p_hwfn, false, "Process kill counter: %d\n", proc_kill_cnt);

	OSAL_SCHEDULE_RECOVERY_HANDLER(p_hwfn);
}

static void ecore_mcp_send_protocol_stats(struct ecore_hwfn *p_hwfn,
					  struct ecore_ptt *p_ptt,
					  enum MFW_DRV_MSG_TYPE type)
{
	enum ecore_mcp_protocol_type stats_type;
	union ecore_mcp_protocol_stats stats;
	struct ecore_mcp_mb_params mb_params;
	union drv_union_data union_data;
	u32 hsi_param;

	switch (type) {
	case MFW_DRV_MSG_GET_LAN_STATS:
		stats_type = ECORE_MCP_LAN_STATS;
		hsi_param = DRV_MSG_CODE_STATS_TYPE_LAN;
		break;
	default:
		DP_NOTICE(p_hwfn, false, "Invalid protocol type %d\n", type);
		return;
	}

	OSAL_GET_PROTOCOL_STATS(p_hwfn->p_dev, stats_type, &stats);

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = DRV_MSG_CODE_GET_STATS;
	mb_params.param = hsi_param;
	OSAL_MEMCPY(&union_data, &stats, sizeof(stats));
	mb_params.p_data_src = &union_data;
	ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);
}

static void
ecore_read_pf_bandwidth(struct ecore_hwfn *p_hwfn,
			struct public_func *p_shmem_info)
{
	struct ecore_mcp_function_info *p_info;

	p_info = &p_hwfn->mcp_info->func_info;

	/* TODO - bandwidth min/max should have valid values of 1-100,
	 * as well as some indication that the feature is disabled.
	 * Until MFW/qlediag enforce those limitations, Assume THERE IS ALWAYS
	 * limit and correct value to min `1' and max `100' if limit isn't in
	 * range.
	 */
	p_info->bandwidth_min = (p_shmem_info->config &
				 FUNC_MF_CFG_MIN_BW_MASK) >>
	    FUNC_MF_CFG_MIN_BW_SHIFT;
	if (p_info->bandwidth_min < 1 || p_info->bandwidth_min > 100) {
		DP_INFO(p_hwfn,
			"bandwidth minimum out of bounds [%02x]. Set to 1\n",
			p_info->bandwidth_min);
		p_info->bandwidth_min = 1;
	}

	p_info->bandwidth_max = (p_shmem_info->config &
				 FUNC_MF_CFG_MAX_BW_MASK) >>
	    FUNC_MF_CFG_MAX_BW_SHIFT;
	if (p_info->bandwidth_max < 1 || p_info->bandwidth_max > 100) {
		DP_INFO(p_hwfn,
			"bandwidth maximum out of bounds [%02x]. Set to 100\n",
			p_info->bandwidth_max);
		p_info->bandwidth_max = 100;
	}
}

static u32 ecore_mcp_get_shmem_func(struct ecore_hwfn *p_hwfn,
				    struct ecore_ptt *p_ptt,
				    struct public_func *p_data,
				    int pfid)
{
	u32 addr = SECTION_OFFSIZE_ADDR(p_hwfn->mcp_info->public_base,
					PUBLIC_FUNC);
	u32 mfw_path_offsize = ecore_rd(p_hwfn, p_ptt, addr);
	u32 func_addr = SECTION_ADDR(mfw_path_offsize, pfid);
	u32 i, size;

	OSAL_MEM_ZERO(p_data, sizeof(*p_data));

	size = OSAL_MIN_T(u32, sizeof(*p_data),
			  SECTION_SIZE(mfw_path_offsize));
	for (i = 0; i < size / sizeof(u32); i++)
		((u32 *)p_data)[i] = ecore_rd(p_hwfn, p_ptt,
					      func_addr + (i << 2));

	return size;
}

static void
ecore_mcp_update_bw(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt)
{
	struct ecore_mcp_function_info *p_info;
	struct public_func shmem_info;
	u32 resp = 0, param = 0;

	ecore_mcp_get_shmem_func(p_hwfn, p_ptt, &shmem_info, MCP_PF_ID(p_hwfn));

	ecore_read_pf_bandwidth(p_hwfn, &shmem_info);

	p_info = &p_hwfn->mcp_info->func_info;

	ecore_configure_pf_min_bandwidth(p_hwfn->p_dev, p_info->bandwidth_min);

	ecore_configure_pf_max_bandwidth(p_hwfn->p_dev, p_info->bandwidth_max);

	/* Acknowledge the MFW */
	ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_BW_UPDATE_ACK, 0, &resp,
		      &param);
}

static void ecore_mcp_handle_fan_failure(struct ecore_hwfn *p_hwfn,
					 struct ecore_ptt *p_ptt)
{
	/* A single notification should be sent to upper driver in CMT mode */
	if (p_hwfn != ECORE_LEADING_HWFN(p_hwfn->p_dev))
		return;

	DP_NOTICE(p_hwfn, false,
		  "Fan failure was detected on the network interface card"
		  " and it's going to be shut down.\n");

	ecore_hw_err_notify(p_hwfn, ECORE_HW_ERR_FAN_FAIL);
}

static enum _ecore_status_t
ecore_mcp_mdump_cmd(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt,
		    u32 mdump_cmd, union drv_union_data *p_data_src,
		    union drv_union_data *p_data_dst, u32 *p_mcp_resp)
{
	struct ecore_mcp_mb_params mb_params;
	enum _ecore_status_t rc;

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = DRV_MSG_CODE_MDUMP_CMD;
	mb_params.param = mdump_cmd;
	mb_params.p_data_src = p_data_src;
	mb_params.p_data_dst = p_data_dst;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);
	if (rc != ECORE_SUCCESS)
		return rc;

	*p_mcp_resp = mb_params.mcp_resp;
	if (*p_mcp_resp == FW_MSG_CODE_MDUMP_INVALID_CMD) {
		DP_NOTICE(p_hwfn, false,
			  "MFW claims that the mdump command is illegal [mdump_cmd 0x%x]\n",
			  mdump_cmd);
		rc = ECORE_INVAL;
	}

	return rc;
}

static enum _ecore_status_t ecore_mcp_mdump_ack(struct ecore_hwfn *p_hwfn,
						struct ecore_ptt *p_ptt)
{
	u32 mcp_resp;

	return ecore_mcp_mdump_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_MDUMP_ACK,
				   OSAL_NULL, OSAL_NULL, &mcp_resp);
}

enum _ecore_status_t ecore_mcp_mdump_set_values(struct ecore_hwfn *p_hwfn,
						struct ecore_ptt *p_ptt,
						u32 epoch)
{
	union drv_union_data union_data;
	u32 mcp_resp;

	OSAL_MEMCPY(&union_data.raw_data, &epoch, sizeof(epoch));

	return ecore_mcp_mdump_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_MDUMP_SET_VALUES,
				   &union_data, OSAL_NULL, &mcp_resp);
}

enum _ecore_status_t ecore_mcp_mdump_trigger(struct ecore_hwfn *p_hwfn,
					     struct ecore_ptt *p_ptt)
{
	u32 mcp_resp;

	return ecore_mcp_mdump_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_MDUMP_TRIGGER,
				   OSAL_NULL, OSAL_NULL, &mcp_resp);
}

enum _ecore_status_t ecore_mcp_mdump_clear_logs(struct ecore_hwfn *p_hwfn,
						struct ecore_ptt *p_ptt)
{
	u32 mcp_resp;

	return ecore_mcp_mdump_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_MDUMP_CLEAR_LOGS,
				   OSAL_NULL, OSAL_NULL, &mcp_resp);
}

static enum _ecore_status_t
ecore_mcp_mdump_get_config(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt,
			   struct mdump_config_stc *p_mdump_config)
{
	union drv_union_data union_data;
	u32 mcp_resp;
	enum _ecore_status_t rc;

	rc = ecore_mcp_mdump_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_MDUMP_GET_CONFIG,
				 OSAL_NULL, &union_data, &mcp_resp);
	if (rc != ECORE_SUCCESS)
		return rc;

	/* A zero response implies that the mdump command is not supported */
	if (!mcp_resp)
		return ECORE_NOTIMPL;

	if (mcp_resp != FW_MSG_CODE_OK) {
		DP_NOTICE(p_hwfn, false,
			  "Failed to get the mdump configuration and logs info [mcp_resp 0x%x]\n",
			  mcp_resp);
		rc = ECORE_UNKNOWN_ERROR;
	}

	OSAL_MEMCPY(p_mdump_config, &union_data.mdump_config,
		    sizeof(*p_mdump_config));

	return rc;
}

enum _ecore_status_t ecore_mcp_mdump_get_info(struct ecore_hwfn *p_hwfn,
					      struct ecore_ptt *p_ptt)
{
	struct mdump_config_stc mdump_config;
	enum _ecore_status_t rc;

	rc = ecore_mcp_mdump_get_config(p_hwfn, p_ptt, &mdump_config);
	if (rc != ECORE_SUCCESS)
		return rc;

	DP_VERBOSE(p_hwfn, ECORE_MSG_SP,
		   "MFW mdump_config: version 0x%x, config 0x%x, epoch 0x%x, num_of_logs 0x%x, valid_logs 0x%x\n",
		   mdump_config.version, mdump_config.config, mdump_config.epoc,
		   mdump_config.num_of_logs, mdump_config.valid_logs);

	if (mdump_config.valid_logs > 0) {
		DP_NOTICE(p_hwfn, false,
			  "* * * IMPORTANT - HW ERROR register dump captured by device * * *\n");
	}

	return rc;
}

void ecore_mcp_mdump_enable(struct ecore_dev *p_dev, bool mdump_enable)
{
	p_dev->mdump_en = mdump_enable;
}

static void ecore_mcp_handle_critical_error(struct ecore_hwfn *p_hwfn,
					    struct ecore_ptt *p_ptt)
{
	/* In CMT mode - no need for more than a single acknowledgment to the
	 * MFW, and no more than a single notification to the upper driver.
	 */
	if (p_hwfn != ECORE_LEADING_HWFN(p_hwfn->p_dev))
		return;

	DP_NOTICE(p_hwfn, false,
		  "Received a critical error notification from the MFW!\n");

	if (p_hwfn->p_dev->mdump_en) {
		DP_NOTICE(p_hwfn, false,
			  "Not acknowledging the notification to allow the MFW crash dump\n");
		return;
	}

	ecore_mcp_mdump_ack(p_hwfn, p_ptt);
	ecore_hw_err_notify(p_hwfn, ECORE_HW_ERR_HW_ATTN);
}

enum _ecore_status_t ecore_mcp_handle_events(struct ecore_hwfn *p_hwfn,
					     struct ecore_ptt *p_ptt)
{
	struct ecore_mcp_info *info = p_hwfn->mcp_info;
	enum _ecore_status_t rc = ECORE_SUCCESS;
	bool found = false;
	u16 i;

	DP_VERBOSE(p_hwfn, ECORE_MSG_SP, "Received message from MFW\n");

	/* Read Messages from MFW */
	ecore_mcp_read_mb(p_hwfn, p_ptt);

	/* Compare current messages to old ones */
	for (i = 0; i < info->mfw_mb_length; i++) {
		if (info->mfw_mb_cur[i] == info->mfw_mb_shadow[i])
			continue;

		found = true;

		DP_VERBOSE(p_hwfn, ECORE_MSG_LINK,
			   "Msg [%d] - old CMD 0x%02x, new CMD 0x%02x\n",
			   i, info->mfw_mb_shadow[i], info->mfw_mb_cur[i]);

		switch (i) {
		case MFW_DRV_MSG_LINK_CHANGE:
			ecore_mcp_handle_link_change(p_hwfn, p_ptt, false);
			break;
		case MFW_DRV_MSG_VF_DISABLED:
			ecore_mcp_handle_vf_flr(p_hwfn, p_ptt);
			break;
		case MFW_DRV_MSG_LLDP_DATA_UPDATED:
			ecore_dcbx_mib_update_event(p_hwfn, p_ptt,
						    ECORE_DCBX_REMOTE_LLDP_MIB);
			break;
		case MFW_DRV_MSG_DCBX_REMOTE_MIB_UPDATED:
			ecore_dcbx_mib_update_event(p_hwfn, p_ptt,
						    ECORE_DCBX_REMOTE_MIB);
			break;
		case MFW_DRV_MSG_DCBX_OPERATIONAL_MIB_UPDATED:
			ecore_dcbx_mib_update_event(p_hwfn, p_ptt,
						    ECORE_DCBX_OPERATIONAL_MIB);
			break;
		case MFW_DRV_MSG_TRANSCEIVER_STATE_CHANGE:
			ecore_mcp_handle_transceiver_change(p_hwfn, p_ptt);
			break;
		case MFW_DRV_MSG_ERROR_RECOVERY:
			ecore_mcp_handle_process_kill(p_hwfn, p_ptt);
			break;
		case MFW_DRV_MSG_GET_LAN_STATS:
		case MFW_DRV_MSG_GET_FCOE_STATS:
		case MFW_DRV_MSG_GET_ISCSI_STATS:
		case MFW_DRV_MSG_GET_RDMA_STATS:
			ecore_mcp_send_protocol_stats(p_hwfn, p_ptt, i);
			break;
		case MFW_DRV_MSG_BW_UPDATE:
			ecore_mcp_update_bw(p_hwfn, p_ptt);
			break;
		case MFW_DRV_MSG_FAILURE_DETECTED:
			ecore_mcp_handle_fan_failure(p_hwfn, p_ptt);
			break;
		case MFW_DRV_MSG_CRITICAL_ERROR_OCCURRED:
			ecore_mcp_handle_critical_error(p_hwfn, p_ptt);
			break;
		default:
			/* @DPDK */
			DP_NOTICE(p_hwfn, false,
				  "Unimplemented MFW message %d\n", i);
			rc = ECORE_INVAL;
		}
	}

	/* ACK everything */
	for (i = 0; i < MFW_DRV_MSG_MAX_DWORDS(info->mfw_mb_length); i++) {
		OSAL_BE32 val = OSAL_CPU_TO_BE32(((u32 *)info->mfw_mb_cur)[i]);

		/* MFW expect answer in BE, so we force write in that format */
		ecore_wr(p_hwfn, p_ptt,
			 info->mfw_mb_addr + sizeof(u32) +
			 MFW_DRV_MSG_MAX_DWORDS(info->mfw_mb_length) *
			 sizeof(u32) + i * sizeof(u32), val);
	}

	if (!found) {
		DP_NOTICE(p_hwfn, false,
			  "Received an MFW message indication but no"
			  " new message!\n");
		rc = ECORE_INVAL;
	}

	/* Copy the new mfw messages into the shadow */
	OSAL_MEMCPY(info->mfw_mb_shadow, info->mfw_mb_cur, info->mfw_mb_length);

	return rc;
}

enum _ecore_status_t ecore_mcp_get_mfw_ver(struct ecore_hwfn *p_hwfn,
					   struct ecore_ptt *p_ptt,
					   u32 *p_mfw_ver,
					   u32 *p_running_bundle_id)
{
	u32 global_offsize;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_EMUL(p_hwfn->p_dev)) {
		DP_NOTICE(p_hwfn, false, "Emulation - can't get MFW version\n");
		return ECORE_SUCCESS;
	}
#endif

	if (IS_VF(p_hwfn->p_dev)) {
		if (p_hwfn->vf_iov_info) {
			struct pfvf_acquire_resp_tlv *p_resp;

			p_resp = &p_hwfn->vf_iov_info->acquire_resp;
			*p_mfw_ver = p_resp->pfdev_info.mfw_ver;
			return ECORE_SUCCESS;
		} else {
			DP_VERBOSE(p_hwfn, ECORE_MSG_IOV,
				   "VF requested MFW version prior to ACQUIRE\n");
			return ECORE_INVAL;
		}
	}

	global_offsize = ecore_rd(p_hwfn, p_ptt,
				  SECTION_OFFSIZE_ADDR(p_hwfn->mcp_info->
						       public_base,
						       PUBLIC_GLOBAL));
	*p_mfw_ver =
	    ecore_rd(p_hwfn, p_ptt,
		     SECTION_ADDR(global_offsize,
				  0) + OFFSETOF(struct public_global, mfw_ver));

	if (p_running_bundle_id != OSAL_NULL) {
		*p_running_bundle_id = ecore_rd(p_hwfn, p_ptt,
						SECTION_ADDR(global_offsize,
							     0) +
						OFFSETOF(struct public_global,
							 running_bundle_id));
	}

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_get_media_type(struct ecore_dev *p_dev,
					      u32 *p_media_type)
{
	struct ecore_hwfn *p_hwfn = &p_dev->hwfns[0];
	struct ecore_ptt *p_ptt;

	/* TODO - Add support for VFs */
	if (IS_VF(p_dev))
		return ECORE_INVAL;

	if (!ecore_mcp_is_init(p_hwfn)) {
		DP_NOTICE(p_hwfn, true, "MFW is not initialized !\n");
		return ECORE_BUSY;
	}

	*p_media_type = MEDIA_UNSPECIFIED;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt)
		return ECORE_BUSY;

	*p_media_type = ecore_rd(p_hwfn, p_ptt, p_hwfn->mcp_info->port_addr +
				 OFFSETOF(struct public_port, media_type));

	ecore_ptt_release(p_hwfn, p_ptt);

	return ECORE_SUCCESS;
}

static enum _ecore_status_t
ecore_mcp_get_shmem_proto(struct ecore_hwfn *p_hwfn,
			  struct public_func *p_info,
			  enum ecore_pci_personality *p_proto)
{
	enum _ecore_status_t rc = ECORE_SUCCESS;

	switch (p_info->config & FUNC_MF_CFG_PROTOCOL_MASK) {
	case FUNC_MF_CFG_PROTOCOL_ETHERNET:
		*p_proto = ECORE_PCI_ETH;
		break;
	default:
		rc = ECORE_INVAL;
	}

	return rc;
}

enum _ecore_status_t ecore_mcp_fill_shmem_func_info(struct ecore_hwfn *p_hwfn,
						    struct ecore_ptt *p_ptt)
{
	struct ecore_mcp_function_info *info;
	struct public_func shmem_info;

	ecore_mcp_get_shmem_func(p_hwfn, p_ptt, &shmem_info, MCP_PF_ID(p_hwfn));
	info = &p_hwfn->mcp_info->func_info;

	info->pause_on_host = (shmem_info.config &
			       FUNC_MF_CFG_PAUSE_ON_HOST_RING) ? 1 : 0;

	if (ecore_mcp_get_shmem_proto(p_hwfn, &shmem_info, &info->protocol)) {
		DP_ERR(p_hwfn, "Unknown personality %08x\n",
		       (u32)(shmem_info.config & FUNC_MF_CFG_PROTOCOL_MASK));
		return ECORE_INVAL;
	}

	ecore_read_pf_bandwidth(p_hwfn, &shmem_info);

	if (shmem_info.mac_upper || shmem_info.mac_lower) {
		info->mac[0] = (u8)(shmem_info.mac_upper >> 8);
		info->mac[1] = (u8)(shmem_info.mac_upper);
		info->mac[2] = (u8)(shmem_info.mac_lower >> 24);
		info->mac[3] = (u8)(shmem_info.mac_lower >> 16);
		info->mac[4] = (u8)(shmem_info.mac_lower >> 8);
		info->mac[5] = (u8)(shmem_info.mac_lower);
	} else {
		/* TODO - are there protocols for which there's no MAC? */
		DP_NOTICE(p_hwfn, false, "MAC is 0 in shmem\n");
	}

	/* TODO - are these calculations true for BE machine? */
	info->wwn_port = (u64)shmem_info.fcoe_wwn_port_name_upper |
			 (((u64)shmem_info.fcoe_wwn_port_name_lower) << 32);
	info->wwn_node = (u64)shmem_info.fcoe_wwn_node_name_upper |
			 (((u64)shmem_info.fcoe_wwn_node_name_lower) << 32);

	info->ovlan = (u16)(shmem_info.ovlan_stag & FUNC_MF_CFG_OV_STAG_MASK);

	DP_VERBOSE(p_hwfn, (ECORE_MSG_SP | ECORE_MSG_IFUP),
		   "Read configuration from shmem: pause_on_host %02x"
		    " protocol %02x BW [%02x - %02x]"
		    " MAC %02x:%02x:%02x:%02x:%02x:%02x wwn port %lx"
		    " node %lx ovlan %04x\n",
		   info->pause_on_host, info->protocol,
		   info->bandwidth_min, info->bandwidth_max,
		   info->mac[0], info->mac[1], info->mac[2],
		   info->mac[3], info->mac[4], info->mac[5],
		   (unsigned long)info->wwn_port,
		   (unsigned long)info->wwn_node, info->ovlan);

	return ECORE_SUCCESS;
}

struct ecore_mcp_link_params
*ecore_mcp_get_link_params(struct ecore_hwfn *p_hwfn)
{
	if (!p_hwfn || !p_hwfn->mcp_info)
		return OSAL_NULL;
	return &p_hwfn->mcp_info->link_input;
}

struct ecore_mcp_link_state
*ecore_mcp_get_link_state(struct ecore_hwfn *p_hwfn)
{
	if (!p_hwfn || !p_hwfn->mcp_info)
		return OSAL_NULL;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_SLOW(p_hwfn->p_dev)) {
		DP_INFO(p_hwfn, "Non-ASIC - always notify that link is up\n");
		p_hwfn->mcp_info->link_output.link_up = true;
	}
#endif

	return &p_hwfn->mcp_info->link_output;
}

struct ecore_mcp_link_capabilities
*ecore_mcp_get_link_capabilities(struct ecore_hwfn *p_hwfn)
{
	if (!p_hwfn || !p_hwfn->mcp_info)
		return OSAL_NULL;
	return &p_hwfn->mcp_info->link_capabilities;
}

enum _ecore_status_t ecore_mcp_drain(struct ecore_hwfn *p_hwfn,
				     struct ecore_ptt *p_ptt)
{
	u32 resp = 0, param = 0;
	enum _ecore_status_t rc;

	rc = ecore_mcp_cmd(p_hwfn, p_ptt,
			   DRV_MSG_CODE_NIG_DRAIN, 1000, &resp, &param);

	/* Wait for the drain to complete before returning */
	OSAL_MSLEEP(1020);

	return rc;
}

const struct ecore_mcp_function_info
*ecore_mcp_get_function_info(struct ecore_hwfn *p_hwfn)
{
	if (!p_hwfn || !p_hwfn->mcp_info)
		return OSAL_NULL;
	return &p_hwfn->mcp_info->func_info;
}

enum _ecore_status_t ecore_mcp_nvm_command(struct ecore_hwfn *p_hwfn,
					   struct ecore_ptt *p_ptt,
					   struct ecore_mcp_nvm_params *params)
{
	enum _ecore_status_t rc;

	switch (params->type) {
	case ECORE_MCP_NVM_RD:
		rc = ecore_mcp_nvm_rd_cmd(p_hwfn, p_ptt, params->nvm_common.cmd,
					  params->nvm_common.offset,
					  &params->nvm_common.resp,
					  &params->nvm_common.param,
					  params->nvm_rd.buf_size,
					  params->nvm_rd.buf);
		break;
	case ECORE_MCP_CMD:
		rc = ecore_mcp_cmd(p_hwfn, p_ptt, params->nvm_common.cmd,
				   params->nvm_common.offset,
				   &params->nvm_common.resp,
				   &params->nvm_common.param);
		break;
	case ECORE_MCP_NVM_WR:
		rc = ecore_mcp_nvm_wr_cmd(p_hwfn, p_ptt, params->nvm_common.cmd,
					  params->nvm_common.offset,
					  &params->nvm_common.resp,
					  &params->nvm_common.param,
					  params->nvm_wr.buf_size,
					  params->nvm_wr.buf);
		break;
	default:
		rc = ECORE_NOTIMPL;
		break;
	}
	return rc;
}

int ecore_mcp_get_personality_cnt(struct ecore_hwfn *p_hwfn,
				  struct ecore_ptt *p_ptt, u32 personalities)
{
	enum ecore_pci_personality protocol = ECORE_PCI_DEFAULT;
	struct public_func shmem_info;
	int i, count = 0, num_pfs;

	num_pfs = NUM_OF_ENG_PFS(p_hwfn->p_dev);

	for (i = 0; i < num_pfs; i++) {
		ecore_mcp_get_shmem_func(p_hwfn, p_ptt, &shmem_info,
					 MCP_PF_ID_BY_REL(p_hwfn, i));
		if (shmem_info.config & FUNC_MF_CFG_FUNC_HIDE)
			continue;

		if (ecore_mcp_get_shmem_proto(p_hwfn, &shmem_info,
					      &protocol) != ECORE_SUCCESS)
			continue;

		if ((1 << ((u32)protocol)) & personalities)
			count++;
	}

	return count;
}

enum _ecore_status_t ecore_mcp_get_flash_size(struct ecore_hwfn *p_hwfn,
					      struct ecore_ptt *p_ptt,
					      u32 *p_flash_size)
{
	u32 flash_size;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_EMUL(p_hwfn->p_dev)) {
		DP_NOTICE(p_hwfn, false, "Emulation - can't get flash size\n");
		return ECORE_INVAL;
	}
#endif

	if (IS_VF(p_hwfn->p_dev))
		return ECORE_INVAL;

	flash_size = ecore_rd(p_hwfn, p_ptt, MCP_REG_NVM_CFG4);
	flash_size = (flash_size & MCP_REG_NVM_CFG4_FLASH_SIZE) >>
	    MCP_REG_NVM_CFG4_FLASH_SIZE_SHIFT;
	flash_size = (1 << (flash_size + MCP_BYTES_PER_MBIT_SHIFT));

	*p_flash_size = flash_size;

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_start_recovery_process(struct ecore_hwfn *p_hwfn,
						  struct ecore_ptt *p_ptt)
{
	struct ecore_dev *p_dev = p_hwfn->p_dev;

	if (p_dev->recov_in_prog) {
		DP_NOTICE(p_hwfn, false,
			  "Avoid triggering a recovery since such a process"
			  " is already in progress\n");
		return ECORE_AGAIN;
	}

	DP_NOTICE(p_hwfn, false, "Triggering a recovery process\n");
	ecore_wr(p_hwfn, p_ptt, MISC_REG_AEU_GENERAL_ATTN_35, 0x1);

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_config_vf_msix(struct ecore_hwfn *p_hwfn,
					      struct ecore_ptt *p_ptt,
					      u8 vf_id, u8 num)
{
	u32 resp = 0, param = 0, rc_param = 0;
	enum _ecore_status_t rc;

/* Only Leader can configure MSIX, and need to take CMT into account */

	if (!IS_LEAD_HWFN(p_hwfn))
		return ECORE_SUCCESS;
	num *= p_hwfn->p_dev->num_hwfns;

	param |= (vf_id << DRV_MB_PARAM_CFG_VF_MSIX_VF_ID_SHIFT) &
	    DRV_MB_PARAM_CFG_VF_MSIX_VF_ID_MASK;
	param |= (num << DRV_MB_PARAM_CFG_VF_MSIX_SB_NUM_SHIFT) &
	    DRV_MB_PARAM_CFG_VF_MSIX_SB_NUM_MASK;

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_CFG_VF_MSIX, param,
			   &resp, &rc_param);

	if (resp != FW_MSG_CODE_DRV_CFG_VF_MSIX_DONE) {
		DP_NOTICE(p_hwfn, true, "VF[%d]: MFW failed to set MSI-X\n",
			  vf_id);
		rc = ECORE_INVAL;
	} else {
		DP_VERBOSE(p_hwfn, ECORE_MSG_IOV,
			   "Requested 0x%02x MSI-x interrupts from VF 0x%02x\n",
			    num, vf_id);
	}

	return rc;
}

enum _ecore_status_t
ecore_mcp_send_drv_version(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt,
			   struct ecore_mcp_drv_version *p_ver)
{
	struct drv_version_stc *p_drv_version;
	struct ecore_mcp_mb_params mb_params;
	union drv_union_data union_data;
	u32 num_words, i;
	void *p_name;
	OSAL_BE32 val;
	enum _ecore_status_t rc;

#ifndef ASIC_ONLY
	if (CHIP_REV_IS_SLOW(p_hwfn->p_dev))
		return ECORE_SUCCESS;
#endif

	p_drv_version = &union_data.drv_version;
	p_drv_version->version = p_ver->version;
	num_words = (MCP_DRV_VER_STR_SIZE - 4) / 4;
	for (i = 0; i < num_words; i++) {
		p_name = &p_ver->name[i * sizeof(u32)];
		val = OSAL_CPU_TO_BE32(*(u32 *)p_name);
		*(u32 *)&p_drv_version->name[i * sizeof(u32)] = val;
	}

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = DRV_MSG_CODE_SET_VERSION;
	mb_params.p_data_src = &union_data;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);
	if (rc != ECORE_SUCCESS)
		DP_ERR(p_hwfn, "MCP response failure, aborting\n");

	return rc;
}

enum _ecore_status_t ecore_mcp_halt(struct ecore_hwfn *p_hwfn,
				    struct ecore_ptt *p_ptt)
{
	enum _ecore_status_t rc;
	u32 resp = 0, param = 0;

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_MCP_HALT, 0, &resp,
			   &param);
	if (rc != ECORE_SUCCESS)
		DP_ERR(p_hwfn, "MCP response failure, aborting\n");

	return rc;
}

enum _ecore_status_t ecore_mcp_resume(struct ecore_hwfn *p_hwfn,
				      struct ecore_ptt *p_ptt)
{
	u32 value, cpu_mode;

	ecore_wr(p_hwfn, p_ptt, MCP_REG_CPU_STATE, 0xffffffff);

	value = ecore_rd(p_hwfn, p_ptt, MCP_REG_CPU_MODE);
	value &= ~MCP_REG_CPU_MODE_SOFT_HALT;
	ecore_wr(p_hwfn, p_ptt, MCP_REG_CPU_MODE, value);
	cpu_mode = ecore_rd(p_hwfn, p_ptt, MCP_REG_CPU_MODE);

	return (cpu_mode & MCP_REG_CPU_MODE_SOFT_HALT) ? -1 : 0;
}

enum _ecore_status_t
ecore_mcp_ov_update_current_config(struct ecore_hwfn *p_hwfn,
				   struct ecore_ptt *p_ptt,
				   enum ecore_ov_config_method config,
				   enum ecore_ov_client client)
{
	enum _ecore_status_t rc;
	u32 resp = 0, param = 0;
	u32 drv_mb_param;

	switch (config) {
	case ECORE_OV_CLIENT_DRV:
		drv_mb_param = DRV_MB_PARAM_OV_CURR_CFG_OS;
		break;
	case ECORE_OV_CLIENT_USER:
		drv_mb_param = DRV_MB_PARAM_OV_CURR_CFG_OTHER;
		break;
	default:
		DP_NOTICE(p_hwfn, true, "Invalid client type %d\n", config);
		return ECORE_INVAL;
	}

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_OV_UPDATE_CURR_CFG,
			   drv_mb_param, &resp, &param);
	if (rc != ECORE_SUCCESS)
		DP_ERR(p_hwfn, "MCP response failure, aborting\n");

	return rc;
}

enum _ecore_status_t
ecore_mcp_ov_update_driver_state(struct ecore_hwfn *p_hwfn,
				 struct ecore_ptt *p_ptt,
				 enum ecore_ov_driver_state drv_state)
{
	enum _ecore_status_t rc;
	u32 resp = 0, param = 0;
	u32 drv_mb_param;

	switch (drv_state) {
	case ECORE_OV_DRIVER_STATE_NOT_LOADED:
		drv_mb_param = DRV_MSG_CODE_OV_UPDATE_DRIVER_STATE_NOT_LOADED;
		break;
	case ECORE_OV_DRIVER_STATE_DISABLED:
		drv_mb_param = DRV_MSG_CODE_OV_UPDATE_DRIVER_STATE_DISABLED;
		break;
	case ECORE_OV_DRIVER_STATE_ACTIVE:
		drv_mb_param = DRV_MSG_CODE_OV_UPDATE_DRIVER_STATE_ACTIVE;
		break;
	default:
		DP_NOTICE(p_hwfn, true, "Invalid driver state %d\n", drv_state);
		return ECORE_INVAL;
	}

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_OV_UPDATE_DRIVER_STATE,
			   drv_state, &resp, &param);
	if (rc != ECORE_SUCCESS)
		DP_ERR(p_hwfn, "MCP response failure, aborting\n");

	return rc;
}

enum _ecore_status_t
ecore_mcp_ov_get_fc_npiv(struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt,
			 struct ecore_fc_npiv_tbl *p_table)
{
	return 0;
}

enum _ecore_status_t
ecore_mcp_ov_update_mtu(struct ecore_hwfn *p_hwfn,
			struct ecore_ptt *p_ptt, u16 mtu)
{
	return 0;
}

enum _ecore_status_t ecore_mcp_set_led(struct ecore_hwfn *p_hwfn,
				       struct ecore_ptt *p_ptt,
				       enum ecore_led_mode mode)
{
	u32 resp = 0, param = 0, drv_mb_param;
	enum _ecore_status_t rc;

	switch (mode) {
	case ECORE_LED_MODE_ON:
		drv_mb_param = DRV_MB_PARAM_SET_LED_MODE_ON;
		break;
	case ECORE_LED_MODE_OFF:
		drv_mb_param = DRV_MB_PARAM_SET_LED_MODE_OFF;
		break;
	case ECORE_LED_MODE_RESTORE:
		drv_mb_param = DRV_MB_PARAM_SET_LED_MODE_OPER;
		break;
	default:
		DP_NOTICE(p_hwfn, true, "Invalid LED mode %d\n", mode);
		return ECORE_INVAL;
	}

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_SET_LED_MODE,
			   drv_mb_param, &resp, &param);
	if (rc != ECORE_SUCCESS)
		DP_ERR(p_hwfn, "MCP response failure, aborting\n");

	return rc;
}

enum _ecore_status_t ecore_mcp_mask_parities(struct ecore_hwfn *p_hwfn,
					     struct ecore_ptt *p_ptt,
					     u32 mask_parities)
{
	enum _ecore_status_t rc;
	u32 resp = 0, param = 0;

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_MASK_PARITIES,
			   mask_parities, &resp, &param);

	if (rc != ECORE_SUCCESS) {
		DP_ERR(p_hwfn,
		       "MCP response failure for mask parities, aborting\n");
	} else if (resp != FW_MSG_CODE_OK) {
		DP_ERR(p_hwfn,
		       "MCP did not ack mask parity request. Old MFW?\n");
		rc = ECORE_INVAL;
	}

	return rc;
}

enum _ecore_status_t ecore_mcp_nvm_read(struct ecore_dev *p_dev, u32 addr,
					u8 *p_buf, u32 len)
{
	struct ecore_hwfn *p_hwfn = ECORE_LEADING_HWFN(p_dev);
	u32 bytes_left, offset, bytes_to_copy, buf_size;
	struct ecore_mcp_nvm_params params;
	struct ecore_ptt *p_ptt;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt)
		return ECORE_BUSY;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	bytes_left = len;
	offset = 0;
	params.type = ECORE_MCP_NVM_RD;
	params.nvm_rd.buf_size = &buf_size;
	params.nvm_common.cmd = DRV_MSG_CODE_NVM_READ_NVRAM;
	while (bytes_left > 0) {
		bytes_to_copy = OSAL_MIN_T(u32, bytes_left,
					   MCP_DRV_NVM_BUF_LEN);
		params.nvm_common.offset = (addr + offset) |
		    (bytes_to_copy << DRV_MB_PARAM_NVM_LEN_SHIFT);
		params.nvm_rd.buf = (u32 *)(p_buf + offset);
		rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
		if (rc != ECORE_SUCCESS || (params.nvm_common.resp !=
					    FW_MSG_CODE_NVM_OK)) {
			DP_NOTICE(p_dev, false, "MCP command rc = %d\n", rc);
			break;
		}

		/* This can be a lengthy process, and it's possible scheduler
		 * isn't preemptible. Sleep a bit to prevent CPU hogging.
		 */
		if (bytes_left % 0x1000 <
		    (bytes_left - *params.nvm_rd.buf_size) % 0x1000)
			OSAL_MSLEEP(1);

		offset += *params.nvm_rd.buf_size;
		bytes_left -= *params.nvm_rd.buf_size;
	}

	p_dev->mcp_nvm_resp = params.nvm_common.resp;
	ecore_ptt_release(p_hwfn, p_ptt);

	return rc;
}

enum _ecore_status_t ecore_mcp_phy_read(struct ecore_dev *p_dev, u32 cmd,
					u32 addr, u8 *p_buf, u32 len)
{
	struct ecore_hwfn *p_hwfn = ECORE_LEADING_HWFN(p_dev);
	struct ecore_mcp_nvm_params params;
	struct ecore_ptt *p_ptt;
	enum _ecore_status_t rc;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt)
		return ECORE_BUSY;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	params.type = ECORE_MCP_NVM_RD;
	params.nvm_rd.buf_size = &len;
	params.nvm_common.cmd = (cmd == ECORE_PHY_CORE_READ) ?
	    DRV_MSG_CODE_PHY_CORE_READ : DRV_MSG_CODE_PHY_RAW_READ;
	params.nvm_common.offset = addr;
	params.nvm_rd.buf = (u32 *)p_buf;
	rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
	if (rc != ECORE_SUCCESS)
		DP_NOTICE(p_dev, false, "MCP command rc = %d\n", rc);

	p_dev->mcp_nvm_resp = params.nvm_common.resp;
	ecore_ptt_release(p_hwfn, p_ptt);

	return rc;
}

enum _ecore_status_t ecore_mcp_nvm_resp(struct ecore_dev *p_dev, u8 *p_buf)
{
	struct ecore_hwfn *p_hwfn = ECORE_LEADING_HWFN(p_dev);
	struct ecore_mcp_nvm_params params;
	struct ecore_ptt *p_ptt;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt)
		return ECORE_BUSY;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	OSAL_MEMCPY(p_buf, &p_dev->mcp_nvm_resp, sizeof(p_dev->mcp_nvm_resp));
	ecore_ptt_release(p_hwfn, p_ptt);

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_nvm_del_file(struct ecore_dev *p_dev, u32 addr)
{
	struct ecore_hwfn *p_hwfn = ECORE_LEADING_HWFN(p_dev);
	struct ecore_mcp_nvm_params params;
	struct ecore_ptt *p_ptt;
	enum _ecore_status_t rc;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt)
		return ECORE_BUSY;
	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	params.type = ECORE_MCP_CMD;
	params.nvm_common.cmd = DRV_MSG_CODE_NVM_DEL_FILE;
	params.nvm_common.offset = addr;
	rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
	p_dev->mcp_nvm_resp = params.nvm_common.resp;
	ecore_ptt_release(p_hwfn, p_ptt);

	return rc;
}

enum _ecore_status_t ecore_mcp_nvm_put_file_begin(struct ecore_dev *p_dev,
						  u32 addr)
{
	struct ecore_hwfn *p_hwfn = ECORE_LEADING_HWFN(p_dev);
	struct ecore_mcp_nvm_params params;
	struct ecore_ptt *p_ptt;
	enum _ecore_status_t rc;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt)
		return ECORE_BUSY;
	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	params.type = ECORE_MCP_CMD;
	params.nvm_common.cmd = DRV_MSG_CODE_NVM_PUT_FILE_BEGIN;
	params.nvm_common.offset = addr;
	rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
	p_dev->mcp_nvm_resp = params.nvm_common.resp;
	ecore_ptt_release(p_hwfn, p_ptt);

	return rc;
}

/* rc receives ECORE_INVAL as default parameter because
 * it might not enter the while loop if the len is 0
 */
enum _ecore_status_t ecore_mcp_nvm_write(struct ecore_dev *p_dev, u32 cmd,
					 u32 addr, u8 *p_buf, u32 len)
{
	struct ecore_hwfn *p_hwfn = ECORE_LEADING_HWFN(p_dev);
	enum _ecore_status_t rc = ECORE_INVAL;
	struct ecore_mcp_nvm_params params;
	struct ecore_ptt *p_ptt;
	u32 buf_idx, buf_size;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt)
		return ECORE_BUSY;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	params.type = ECORE_MCP_NVM_WR;
	if (cmd == ECORE_PUT_FILE_DATA)
		params.nvm_common.cmd = DRV_MSG_CODE_NVM_PUT_FILE_DATA;
	else
		params.nvm_common.cmd = DRV_MSG_CODE_NVM_WRITE_NVRAM;
	buf_idx = 0;
	while (buf_idx < len) {
		buf_size = OSAL_MIN_T(u32, (len - buf_idx),
				      MCP_DRV_NVM_BUF_LEN);
		params.nvm_common.offset = ((buf_size <<
					     DRV_MB_PARAM_NVM_LEN_SHIFT)
					    | addr) + buf_idx;
		params.nvm_wr.buf_size = buf_size;
		params.nvm_wr.buf = (u32 *)&p_buf[buf_idx];
		rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
		if (rc != ECORE_SUCCESS ||
		    ((params.nvm_common.resp != FW_MSG_CODE_NVM_OK) &&
		     (params.nvm_common.resp !=
		      FW_MSG_CODE_NVM_PUT_FILE_FINISH_OK)))
			DP_NOTICE(p_dev, false, "MCP command rc = %d\n", rc);

		/* This can be a lengthy process, and it's possible scheduler
		 * isn't preemptible. Sleep a bit to prevent CPU hogging.
		 */
		if (buf_idx % 0x1000 >
		    (buf_idx + buf_size) % 0x1000)
			OSAL_MSLEEP(1);

		buf_idx += buf_size;
	}

	p_dev->mcp_nvm_resp = params.nvm_common.resp;
	ecore_ptt_release(p_hwfn, p_ptt);

	return rc;
}

enum _ecore_status_t ecore_mcp_phy_write(struct ecore_dev *p_dev, u32 cmd,
					 u32 addr, u8 *p_buf, u32 len)
{
	struct ecore_hwfn *p_hwfn = ECORE_LEADING_HWFN(p_dev);
	struct ecore_mcp_nvm_params params;
	struct ecore_ptt *p_ptt;
	enum _ecore_status_t rc;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt)
		return ECORE_BUSY;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	params.type = ECORE_MCP_NVM_WR;
	params.nvm_wr.buf_size = len;
	params.nvm_common.cmd = (cmd == ECORE_PHY_CORE_WRITE) ?
	    DRV_MSG_CODE_PHY_CORE_WRITE : DRV_MSG_CODE_PHY_RAW_WRITE;
	params.nvm_common.offset = addr;
	params.nvm_wr.buf = (u32 *)p_buf;
	rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
	if (rc != ECORE_SUCCESS)
		DP_NOTICE(p_dev, false, "MCP command rc = %d\n", rc);
	p_dev->mcp_nvm_resp = params.nvm_common.resp;
	ecore_ptt_release(p_hwfn, p_ptt);

	return rc;
}

enum _ecore_status_t ecore_mcp_nvm_set_secure_mode(struct ecore_dev *p_dev,
						   u32 addr)
{
	struct ecore_hwfn *p_hwfn = ECORE_LEADING_HWFN(p_dev);
	struct ecore_mcp_nvm_params params;
	struct ecore_ptt *p_ptt;
	enum _ecore_status_t rc;

	p_ptt = ecore_ptt_acquire(p_hwfn);
	if (!p_ptt)
		return ECORE_BUSY;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	params.type = ECORE_MCP_CMD;
	params.nvm_common.cmd = DRV_MSG_CODE_SET_SECURE_MODE;
	params.nvm_common.offset = addr;
	rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
	p_dev->mcp_nvm_resp = params.nvm_common.resp;
	ecore_ptt_release(p_hwfn, p_ptt);

	return rc;
}

enum _ecore_status_t ecore_mcp_phy_sfp_read(struct ecore_hwfn *p_hwfn,
					    struct ecore_ptt *p_ptt,
					    u32 port, u32 addr, u32 offset,
					    u32 len, u8 *p_buf)
{
	struct ecore_mcp_nvm_params params;
	enum _ecore_status_t rc;
	u32 bytes_left, bytes_to_copy, buf_size;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	params.nvm_common.offset =
		(port << DRV_MB_PARAM_TRANSCEIVER_PORT_SHIFT) |
		(addr << DRV_MB_PARAM_TRANSCEIVER_I2C_ADDRESS_SHIFT);
	addr = offset;
	offset = 0;
	bytes_left = len;
	params.type = ECORE_MCP_NVM_RD;
	params.nvm_rd.buf_size = &buf_size;
	params.nvm_common.cmd = DRV_MSG_CODE_TRANSCEIVER_READ;
	while (bytes_left > 0) {
		bytes_to_copy = OSAL_MIN_T(u32, bytes_left,
					   MAX_I2C_TRANSACTION_SIZE);
		params.nvm_rd.buf = (u32 *)(p_buf + offset);
		params.nvm_common.offset &=
			(DRV_MB_PARAM_TRANSCEIVER_I2C_ADDRESS_MASK |
			 DRV_MB_PARAM_TRANSCEIVER_PORT_MASK);
		params.nvm_common.offset |=
			((addr + offset) <<
			 DRV_MB_PARAM_TRANSCEIVER_OFFSET_SHIFT);
		params.nvm_common.offset |=
			(bytes_to_copy << DRV_MB_PARAM_TRANSCEIVER_SIZE_SHIFT);
		rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
		if ((params.nvm_common.resp & FW_MSG_CODE_MASK) ==
		    FW_MSG_CODE_TRANSCEIVER_NOT_PRESENT) {
			return ECORE_NODEV;
		} else if ((params.nvm_common.resp & FW_MSG_CODE_MASK) !=
			   FW_MSG_CODE_TRANSCEIVER_DIAG_OK)
			return ECORE_UNKNOWN_ERROR;

		offset += *params.nvm_rd.buf_size;
		bytes_left -= *params.nvm_rd.buf_size;
	}

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_phy_sfp_write(struct ecore_hwfn *p_hwfn,
					     struct ecore_ptt *p_ptt,
					     u32 port, u32 addr, u32 offset,
					     u32 len, u8 *p_buf)
{
	struct ecore_mcp_nvm_params params;
	enum _ecore_status_t rc;
	u32 buf_idx, buf_size;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	params.nvm_common.offset =
		(port << DRV_MB_PARAM_TRANSCEIVER_PORT_SHIFT) |
		(addr << DRV_MB_PARAM_TRANSCEIVER_I2C_ADDRESS_SHIFT);
	params.type = ECORE_MCP_NVM_WR;
	params.nvm_common.cmd = DRV_MSG_CODE_TRANSCEIVER_WRITE;
	buf_idx = 0;
	while (buf_idx < len) {
		buf_size = OSAL_MIN_T(u32, (len - buf_idx),
				      MAX_I2C_TRANSACTION_SIZE);
		params.nvm_common.offset &=
			(DRV_MB_PARAM_TRANSCEIVER_I2C_ADDRESS_MASK |
			 DRV_MB_PARAM_TRANSCEIVER_PORT_MASK);
		params.nvm_common.offset |=
			((offset + buf_idx) <<
			 DRV_MB_PARAM_TRANSCEIVER_OFFSET_SHIFT);
		params.nvm_common.offset |=
			(buf_size << DRV_MB_PARAM_TRANSCEIVER_SIZE_SHIFT);
		params.nvm_wr.buf_size = buf_size;
		params.nvm_wr.buf = (u32 *)&p_buf[buf_idx];
		rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
		if ((params.nvm_common.resp & FW_MSG_CODE_MASK) ==
		    FW_MSG_CODE_TRANSCEIVER_NOT_PRESENT) {
			return ECORE_NODEV;
		} else if ((params.nvm_common.resp & FW_MSG_CODE_MASK) !=
			   FW_MSG_CODE_TRANSCEIVER_DIAG_OK)
			return ECORE_UNKNOWN_ERROR;

		buf_idx += buf_size;
	}

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_gpio_read(struct ecore_hwfn *p_hwfn,
					 struct ecore_ptt *p_ptt,
					 u16 gpio, u32 *gpio_val)
{
	enum _ecore_status_t rc = ECORE_SUCCESS;
	u32 drv_mb_param = 0, rsp;

	drv_mb_param = (gpio << DRV_MB_PARAM_GPIO_NUMBER_SHIFT);

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_GPIO_READ,
			   drv_mb_param, &rsp, gpio_val);

	if (rc != ECORE_SUCCESS)
		return rc;

	if ((rsp & FW_MSG_CODE_MASK) != FW_MSG_CODE_GPIO_OK)
		return ECORE_UNKNOWN_ERROR;

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_gpio_write(struct ecore_hwfn *p_hwfn,
					  struct ecore_ptt *p_ptt,
					  u16 gpio, u16 gpio_val)
{
	enum _ecore_status_t rc = ECORE_SUCCESS;
	u32 drv_mb_param = 0, param, rsp;

	drv_mb_param = (gpio << DRV_MB_PARAM_GPIO_NUMBER_SHIFT) |
		(gpio_val << DRV_MB_PARAM_GPIO_VALUE_SHIFT);

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_GPIO_WRITE,
			   drv_mb_param, &rsp, &param);

	if (rc != ECORE_SUCCESS)
		return rc;

	if ((rsp & FW_MSG_CODE_MASK) != FW_MSG_CODE_GPIO_OK)
		return ECORE_UNKNOWN_ERROR;

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_gpio_info(struct ecore_hwfn *p_hwfn,
					 struct ecore_ptt *p_ptt,
					 u16 gpio, u32 *gpio_direction,
					 u32 *gpio_ctrl)
{
	u32 drv_mb_param = 0, rsp, val = 0;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	drv_mb_param = gpio << DRV_MB_PARAM_GPIO_NUMBER_SHIFT;

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_GPIO_INFO,
			   drv_mb_param, &rsp, &val);
	if (rc != ECORE_SUCCESS)
		return rc;

	*gpio_direction = (val & DRV_MB_PARAM_GPIO_DIRECTION_MASK) >>
			   DRV_MB_PARAM_GPIO_DIRECTION_SHIFT;
	*gpio_ctrl = (val & DRV_MB_PARAM_GPIO_CTRL_MASK) >>
		      DRV_MB_PARAM_GPIO_CTRL_SHIFT;

	if ((rsp & FW_MSG_CODE_MASK) != FW_MSG_CODE_GPIO_OK)
		return ECORE_UNKNOWN_ERROR;

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_bist_register_test(struct ecore_hwfn *p_hwfn,
						  struct ecore_ptt *p_ptt)
{
	u32 drv_mb_param = 0, rsp, param;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	drv_mb_param = (DRV_MB_PARAM_BIST_REGISTER_TEST <<
			DRV_MB_PARAM_BIST_TEST_INDEX_SHIFT);

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_BIST_TEST,
			   drv_mb_param, &rsp, &param);

	if (rc != ECORE_SUCCESS)
		return rc;

	if (((rsp & FW_MSG_CODE_MASK) != FW_MSG_CODE_OK) ||
	    (param != DRV_MB_PARAM_BIST_RC_PASSED))
		rc = ECORE_UNKNOWN_ERROR;

	return rc;
}

enum _ecore_status_t ecore_mcp_bist_clock_test(struct ecore_hwfn *p_hwfn,
					       struct ecore_ptt *p_ptt)
{
	u32 drv_mb_param = 0, rsp, param;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	drv_mb_param = (DRV_MB_PARAM_BIST_CLOCK_TEST <<
			DRV_MB_PARAM_BIST_TEST_INDEX_SHIFT);

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_BIST_TEST,
			   drv_mb_param, &rsp, &param);

	if (rc != ECORE_SUCCESS)
		return rc;

	if (((rsp & FW_MSG_CODE_MASK) != FW_MSG_CODE_OK) ||
	    (param != DRV_MB_PARAM_BIST_RC_PASSED))
		rc = ECORE_UNKNOWN_ERROR;

	return rc;
}

enum _ecore_status_t ecore_mcp_bist_nvm_test_get_num_images(
	struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt, u32 *num_images)
{
	u32 drv_mb_param = 0, rsp;
	enum _ecore_status_t rc = ECORE_SUCCESS;

	drv_mb_param = (DRV_MB_PARAM_BIST_NVM_TEST_NUM_IMAGES <<
			DRV_MB_PARAM_BIST_TEST_INDEX_SHIFT);

	rc = ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_BIST_TEST,
			   drv_mb_param, &rsp, num_images);

	if (rc != ECORE_SUCCESS)
		return rc;

	if (((rsp & FW_MSG_CODE_MASK) != FW_MSG_CODE_OK))
		rc = ECORE_UNKNOWN_ERROR;

	return rc;
}

enum _ecore_status_t ecore_mcp_bist_nvm_test_get_image_att(
	struct ecore_hwfn *p_hwfn, struct ecore_ptt *p_ptt,
	struct bist_nvm_image_att *p_image_att, u32 image_index)
{
	struct ecore_mcp_nvm_params params;
	enum _ecore_status_t rc;
	u32 buf_size;

	OSAL_MEMSET(&params, 0, sizeof(struct ecore_mcp_nvm_params));
	params.nvm_common.offset = (DRV_MB_PARAM_BIST_NVM_TEST_IMAGE_BY_INDEX <<
				    DRV_MB_PARAM_BIST_TEST_INDEX_SHIFT);
	params.nvm_common.offset |= (image_index <<
				    DRV_MB_PARAM_BIST_TEST_IMAGE_INDEX_SHIFT);

	params.type = ECORE_MCP_NVM_RD;
	params.nvm_rd.buf_size = &buf_size;
	params.nvm_common.cmd = DRV_MSG_CODE_BIST_TEST;
	params.nvm_rd.buf = (u32 *)p_image_att;

	rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);
	if (rc != ECORE_SUCCESS)
		return rc;

	if (((params.nvm_common.resp & FW_MSG_CODE_MASK) != FW_MSG_CODE_OK) ||
	    (p_image_att->return_code != 1))
		rc = ECORE_UNKNOWN_ERROR;

	return rc;
}

enum _ecore_status_t
ecore_mcp_get_temperature_info(struct ecore_hwfn *p_hwfn,
			       struct ecore_ptt *p_ptt,
			       struct ecore_temperature_info *p_temp_info)
{
	struct ecore_temperature_sensor *p_temp_sensor;
	struct temperature_status_stc *p_mfw_temp_info;
	struct ecore_mcp_mb_params mb_params;
	union drv_union_data union_data;
	u32 val;
	enum _ecore_status_t rc;
	u8 i;

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = DRV_MSG_CODE_GET_TEMPERATURE;
	mb_params.p_data_dst = &union_data;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);
	if (rc != ECORE_SUCCESS)
		return rc;

	p_mfw_temp_info = &union_data.temp_info;

	OSAL_BUILD_BUG_ON(ECORE_MAX_NUM_OF_SENSORS != MAX_NUM_OF_SENSORS);
	p_temp_info->num_sensors = OSAL_MIN_T(u32,
					      p_mfw_temp_info->num_of_sensors,
					      ECORE_MAX_NUM_OF_SENSORS);
	for (i = 0; i < p_temp_info->num_sensors; i++) {
		val = p_mfw_temp_info->sensor[i];
		p_temp_sensor = &p_temp_info->sensors[i];
		p_temp_sensor->sensor_location = (val & SENSOR_LOCATION_MASK) >>
						 SENSOR_LOCATION_SHIFT;
		p_temp_sensor->threshold_high = (val & THRESHOLD_HIGH_MASK) >>
						THRESHOLD_HIGH_SHIFT;
		p_temp_sensor->critical = (val & CRITICAL_TEMPERATURE_MASK) >>
					  CRITICAL_TEMPERATURE_SHIFT;
		p_temp_sensor->current_temp = (val & CURRENT_TEMP_MASK) >>
					      CURRENT_TEMP_SHIFT;
	}

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_get_mba_versions(
	struct ecore_hwfn *p_hwfn,
	struct ecore_ptt *p_ptt,
	struct ecore_mba_vers *p_mba_vers)
{
	struct ecore_mcp_nvm_params params;
	enum _ecore_status_t rc;
	u32 buf_size;

	OSAL_MEM_ZERO(&params, sizeof(params));
	params.type = ECORE_MCP_NVM_RD;
	params.nvm_common.cmd = DRV_MSG_CODE_GET_MBA_VERSION;
	params.nvm_common.offset = 0;
	params.nvm_rd.buf = &p_mba_vers->mba_vers[0];
	params.nvm_rd.buf_size = &buf_size;
	rc = ecore_mcp_nvm_command(p_hwfn, p_ptt, &params);

	if (rc != ECORE_SUCCESS)
		return rc;

	if ((params.nvm_common.resp & FW_MSG_CODE_MASK) !=
	    FW_MSG_CODE_NVM_OK)
		rc = ECORE_UNKNOWN_ERROR;

	if (buf_size != MCP_DRV_NVM_BUF_LEN)
		rc = ECORE_UNKNOWN_ERROR;

	return rc;
}

enum _ecore_status_t ecore_mcp_mem_ecc_events(struct ecore_hwfn *p_hwfn,
					      struct ecore_ptt *p_ptt,
					      u64 *num_events)
{
	u32 rsp;

	return ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_MEM_ECC_EVENTS,
			     0, &rsp, (u32 *)num_events);
}

#define ECORE_RESC_ALLOC_VERSION_MAJOR	1
#define ECORE_RESC_ALLOC_VERSION_MINOR	0
#define ECORE_RESC_ALLOC_VERSION				\
	((ECORE_RESC_ALLOC_VERSION_MAJOR <<			\
	  DRV_MB_PARAM_RESOURCE_ALLOC_VERSION_MAJOR_SHIFT) |	\
	 (ECORE_RESC_ALLOC_VERSION_MINOR <<			\
	  DRV_MB_PARAM_RESOURCE_ALLOC_VERSION_MINOR_SHIFT))

enum _ecore_status_t ecore_mcp_get_resc_info(struct ecore_hwfn *p_hwfn,
					     struct ecore_ptt *p_ptt,
					     struct resource_info *p_resc_info,
					     u32 *p_mcp_resp, u32 *p_mcp_param)
{
	struct ecore_mcp_mb_params mb_params;
	union drv_union_data *p_union_data;
	enum _ecore_status_t rc;

	OSAL_MEM_ZERO(&mb_params, sizeof(mb_params));
	mb_params.cmd = DRV_MSG_GET_RESOURCE_ALLOC_MSG;
	mb_params.param = ECORE_RESC_ALLOC_VERSION;
	p_union_data = (union drv_union_data *)p_resc_info;
	mb_params.p_data_src = p_union_data;
	mb_params.p_data_dst = p_union_data;
	rc = ecore_mcp_cmd_and_union(p_hwfn, p_ptt, &mb_params);
	if (rc != ECORE_SUCCESS)
		return rc;

	*p_mcp_resp = mb_params.mcp_resp;
	*p_mcp_param = mb_params.mcp_param;

	DP_VERBOSE(p_hwfn, ECORE_MSG_SP,
		   "MFW resource_info: version 0x%x, res_id 0x%x, size 0x%x,"
		   " offset 0x%x, vf_size 0x%x, vf_offset 0x%x, flags 0x%x\n",
		   *p_mcp_param, p_resc_info->res_id, p_resc_info->size,
		   p_resc_info->offset, p_resc_info->vf_size,
		   p_resc_info->vf_offset, p_resc_info->flags);

	return ECORE_SUCCESS;
}

enum _ecore_status_t ecore_mcp_initiate_pf_flr(struct ecore_hwfn *p_hwfn,
					       struct ecore_ptt *p_ptt)
{
	u32 mcp_resp, mcp_param;

	return ecore_mcp_cmd(p_hwfn, p_ptt, DRV_MSG_CODE_INITIATE_PF_FLR,
			     0, &mcp_resp, &mcp_param);
}
