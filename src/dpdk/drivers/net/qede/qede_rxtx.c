/*
 * Copyright (c) 2016 QLogic Corporation.
 * All rights reserved.
 * www.qlogic.com
 *
 * See LICENSE.qede_pmd for copyright and licensing details.
 */

#include "qede_rxtx.h"

static bool gro_disable = 1;	/* mod_param */

static inline int qede_alloc_rx_buffer(struct qede_rx_queue *rxq)
{
	struct rte_mbuf *new_mb = NULL;
	struct eth_rx_bd *rx_bd;
	dma_addr_t mapping;
	uint16_t idx = rxq->sw_rx_prod & NUM_RX_BDS(rxq);

	new_mb = rte_mbuf_raw_alloc(rxq->mb_pool);
	if (unlikely(!new_mb)) {
		PMD_RX_LOG(ERR, rxq,
			   "Failed to allocate rx buffer "
			   "sw_rx_prod %u sw_rx_cons %u mp entries %u free %u",
			   idx, rxq->sw_rx_cons & NUM_RX_BDS(rxq),
			   rte_mempool_avail_count(rxq->mb_pool),
			   rte_mempool_in_use_count(rxq->mb_pool));
		return -ENOMEM;
	}
	rxq->sw_rx_ring[idx].mbuf = new_mb;
	rxq->sw_rx_ring[idx].page_offset = 0;
	mapping = rte_mbuf_data_dma_addr_default(new_mb);
	/* Advance PROD and get BD pointer */
	rx_bd = (struct eth_rx_bd *)ecore_chain_produce(&rxq->rx_bd_ring);
	rx_bd->addr.hi = rte_cpu_to_le_32(U64_HI(mapping));
	rx_bd->addr.lo = rte_cpu_to_le_32(U64_LO(mapping));
	rxq->sw_rx_prod++;
	return 0;
}

static void qede_rx_queue_release_mbufs(struct qede_rx_queue *rxq)
{
	uint16_t i;

	if (rxq->sw_rx_ring != NULL) {
		for (i = 0; i < rxq->nb_rx_desc; i++) {
			if (rxq->sw_rx_ring[i].mbuf != NULL) {
				rte_pktmbuf_free(rxq->sw_rx_ring[i].mbuf);
				rxq->sw_rx_ring[i].mbuf = NULL;
			}
		}
	}
}

void qede_rx_queue_release(void *rx_queue)
{
	struct qede_rx_queue *rxq = rx_queue;

	if (rxq != NULL) {
		qede_rx_queue_release_mbufs(rxq);
		rte_free(rxq->sw_rx_ring);
		rxq->sw_rx_ring = NULL;
		rte_free(rxq);
		rxq = NULL;
	}
}

static void qede_tx_queue_release_mbufs(struct qede_tx_queue *txq)
{
	unsigned int i;

	PMD_TX_LOG(DEBUG, txq, "releasing %u mbufs\n", txq->nb_tx_desc);

	if (txq->sw_tx_ring) {
		for (i = 0; i < txq->nb_tx_desc; i++) {
			if (txq->sw_tx_ring[i].mbuf) {
				rte_pktmbuf_free(txq->sw_tx_ring[i].mbuf);
				txq->sw_tx_ring[i].mbuf = NULL;
			}
		}
	}
}

int
qede_rx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
		    uint16_t nb_desc, unsigned int socket_id,
		    const struct rte_eth_rxconf *rx_conf,
		    struct rte_mempool *mp)
{
	struct qede_dev *qdev = dev->data->dev_private;
	struct ecore_dev *edev = &qdev->edev;
	struct rte_eth_dev_data *eth_data = dev->data;
	struct qede_rx_queue *rxq;
	uint16_t pkt_len = (uint16_t)dev->data->dev_conf.rxmode.max_rx_pkt_len;
	size_t size;
	uint16_t data_size;
	int rc;
	int i;

	PMD_INIT_FUNC_TRACE(edev);

	/* Note: Ring size/align is controlled by struct rte_eth_desc_lim */
	if (!rte_is_power_of_2(nb_desc)) {
		DP_ERR(edev, "Ring size %u is not power of 2\n",
			  nb_desc);
		return -EINVAL;
	}

	/* Free memory prior to re-allocation if needed... */
	if (dev->data->rx_queues[queue_idx] != NULL) {
		qede_rx_queue_release(dev->data->rx_queues[queue_idx]);
		dev->data->rx_queues[queue_idx] = NULL;
	}

	/* First allocate the rx queue data structure */
	rxq = rte_zmalloc_socket("qede_rx_queue", sizeof(struct qede_rx_queue),
				 RTE_CACHE_LINE_SIZE, socket_id);

	if (!rxq) {
		DP_ERR(edev, "Unable to allocate memory for rxq on socket %u",
			  socket_id);
		return -ENOMEM;
	}

	rxq->qdev = qdev;
	rxq->mb_pool = mp;
	rxq->nb_rx_desc = nb_desc;
	rxq->queue_id = queue_idx;
	rxq->port_id = dev->data->port_id;

	/* Sanity check */
	data_size = (uint16_t)rte_pktmbuf_data_room_size(mp) -
				RTE_PKTMBUF_HEADROOM;

	if (pkt_len > data_size && !dev->data->scattered_rx) {
		DP_ERR(edev, "MTU %u should not exceed dataroom %u\n",
		       pkt_len, data_size);
		rte_free(rxq);
		return -EINVAL;
	}

	if (dev->data->scattered_rx)
		rxq->rx_buf_size = data_size;
	else
		rxq->rx_buf_size = pkt_len + QEDE_ETH_OVERHEAD;

	qdev->mtu = pkt_len;

	DP_INFO(edev, "MTU = %u ; RX buffer = %u\n",
		qdev->mtu, rxq->rx_buf_size);

	if (pkt_len > ETHER_MAX_LEN) {
		dev->data->dev_conf.rxmode.jumbo_frame = 1;
		DP_NOTICE(edev, false, "jumbo frame enabled\n");
	} else {
		dev->data->dev_conf.rxmode.jumbo_frame = 0;
	}

	/* Allocate the parallel driver ring for Rx buffers */
	size = sizeof(*rxq->sw_rx_ring) * rxq->nb_rx_desc;
	rxq->sw_rx_ring = rte_zmalloc_socket("sw_rx_ring", size,
					     RTE_CACHE_LINE_SIZE, socket_id);
	if (!rxq->sw_rx_ring) {
		DP_NOTICE(edev, false,
			  "Unable to alloc memory for sw_rx_ring on socket %u\n",
			  socket_id);
		rte_free(rxq);
		rxq = NULL;
		return -ENOMEM;
	}

	/* Allocate FW Rx ring  */
	rc = qdev->ops->common->chain_alloc(edev,
					    ECORE_CHAIN_USE_TO_CONSUME_PRODUCE,
					    ECORE_CHAIN_MODE_NEXT_PTR,
					    ECORE_CHAIN_CNT_TYPE_U16,
					    rxq->nb_rx_desc,
					    sizeof(struct eth_rx_bd),
					    &rxq->rx_bd_ring);

	if (rc != ECORE_SUCCESS) {
		DP_NOTICE(edev, false,
			  "Unable to alloc memory for rxbd ring on socket %u\n",
			  socket_id);
		rte_free(rxq->sw_rx_ring);
		rxq->sw_rx_ring = NULL;
		rte_free(rxq);
		rxq = NULL;
		return -ENOMEM;
	}

	/* Allocate FW completion ring */
	rc = qdev->ops->common->chain_alloc(edev,
					    ECORE_CHAIN_USE_TO_CONSUME,
					    ECORE_CHAIN_MODE_PBL,
					    ECORE_CHAIN_CNT_TYPE_U16,
					    rxq->nb_rx_desc,
					    sizeof(union eth_rx_cqe),
					    &rxq->rx_comp_ring);

	if (rc != ECORE_SUCCESS) {
		DP_NOTICE(edev, false,
			  "Unable to alloc memory for cqe ring on socket %u\n",
			  socket_id);
		/* TBD: Freeing RX BD ring */
		rte_free(rxq->sw_rx_ring);
		rxq->sw_rx_ring = NULL;
		rte_free(rxq);
		return -ENOMEM;
	}

	/* Allocate buffers for the Rx ring */
	for (i = 0; i < rxq->nb_rx_desc; i++) {
		rc = qede_alloc_rx_buffer(rxq);
		if (rc) {
			DP_NOTICE(edev, false,
				  "RX buffer allocation failed at idx=%d\n", i);
			goto err4;
		}
	}

	dev->data->rx_queues[queue_idx] = rxq;

	DP_INFO(edev, "rxq %d num_desc %u rx_buf_size=%u socket %u\n",
		  queue_idx, nb_desc, qdev->mtu, socket_id);

	return 0;
err4:
	qede_rx_queue_release(rxq);
	return -ENOMEM;
}

void qede_tx_queue_release(void *tx_queue)
{
	struct qede_tx_queue *txq = tx_queue;

	if (txq != NULL) {
		qede_tx_queue_release_mbufs(txq);
		if (txq->sw_tx_ring) {
			rte_free(txq->sw_tx_ring);
			txq->sw_tx_ring = NULL;
		}
		rte_free(txq);
	}
	txq = NULL;
}

int
qede_tx_queue_setup(struct rte_eth_dev *dev,
		    uint16_t queue_idx,
		    uint16_t nb_desc,
		    unsigned int socket_id,
		    const struct rte_eth_txconf *tx_conf)
{
	struct qede_dev *qdev = dev->data->dev_private;
	struct ecore_dev *edev = &qdev->edev;
	struct qede_tx_queue *txq;
	int rc;

	PMD_INIT_FUNC_TRACE(edev);

	if (!rte_is_power_of_2(nb_desc)) {
		DP_ERR(edev, "Ring size %u is not power of 2\n",
		       nb_desc);
		return -EINVAL;
	}

	/* Free memory prior to re-allocation if needed... */
	if (dev->data->tx_queues[queue_idx] != NULL) {
		qede_tx_queue_release(dev->data->tx_queues[queue_idx]);
		dev->data->tx_queues[queue_idx] = NULL;
	}

	txq = rte_zmalloc_socket("qede_tx_queue", sizeof(struct qede_tx_queue),
				 RTE_CACHE_LINE_SIZE, socket_id);

	if (txq == NULL) {
		DP_ERR(edev,
		       "Unable to allocate memory for txq on socket %u",
		       socket_id);
		return -ENOMEM;
	}

	txq->nb_tx_desc = nb_desc;
	txq->qdev = qdev;
	txq->port_id = dev->data->port_id;

	rc = qdev->ops->common->chain_alloc(edev,
					    ECORE_CHAIN_USE_TO_CONSUME_PRODUCE,
					    ECORE_CHAIN_MODE_PBL,
					    ECORE_CHAIN_CNT_TYPE_U16,
					    txq->nb_tx_desc,
					    sizeof(union eth_tx_bd_types),
					    &txq->tx_pbl);
	if (rc != ECORE_SUCCESS) {
		DP_ERR(edev,
		       "Unable to allocate memory for txbd ring on socket %u",
		       socket_id);
		qede_tx_queue_release(txq);
		return -ENOMEM;
	}

	/* Allocate software ring */
	txq->sw_tx_ring = rte_zmalloc_socket("txq->sw_tx_ring",
					     (sizeof(struct qede_tx_entry) *
					      txq->nb_tx_desc),
					     RTE_CACHE_LINE_SIZE, socket_id);

	if (!txq->sw_tx_ring) {
		DP_ERR(edev,
		       "Unable to allocate memory for txbd ring on socket %u",
		       socket_id);
		qede_tx_queue_release(txq);
		return -ENOMEM;
	}

	txq->queue_id = queue_idx;

	txq->nb_tx_avail = txq->nb_tx_desc;

	txq->tx_free_thresh =
	    tx_conf->tx_free_thresh ? tx_conf->tx_free_thresh :
	    (txq->nb_tx_desc - QEDE_DEFAULT_TX_FREE_THRESH);

	dev->data->tx_queues[queue_idx] = txq;

	DP_INFO(edev,
		  "txq %u num_desc %u tx_free_thresh %u socket %u\n",
		  queue_idx, nb_desc, txq->tx_free_thresh, socket_id);

	return 0;
}

/* This function inits fp content and resets the SB, RXQ and TXQ arrays */
static void qede_init_fp(struct qede_dev *qdev)
{
	struct qede_fastpath *fp;
	uint8_t i, rss_id, tc;
	int fp_rx = qdev->fp_num_rx, rxq = 0, txq = 0;

	memset((void *)qdev->fp_array, 0, (QEDE_QUEUE_CNT(qdev) *
					   sizeof(*qdev->fp_array)));
	memset((void *)qdev->sb_array, 0, (QEDE_QUEUE_CNT(qdev) *
					   sizeof(*qdev->sb_array)));
	for_each_queue(i) {
		fp = &qdev->fp_array[i];
		if (fp_rx) {
			fp->type = QEDE_FASTPATH_RX;
			fp_rx--;
		} else{
			fp->type = QEDE_FASTPATH_TX;
		}
		fp->qdev = qdev;
		fp->id = i;
		fp->sb_info = &qdev->sb_array[i];
		snprintf(fp->name, sizeof(fp->name), "%s-fp-%d", "qdev", i);
	}

	qdev->gro_disable = gro_disable;
}

void qede_free_fp_arrays(struct qede_dev *qdev)
{
	/* It asseumes qede_free_mem_load() is called before */
	if (qdev->fp_array != NULL) {
		rte_free(qdev->fp_array);
		qdev->fp_array = NULL;
	}

	if (qdev->sb_array != NULL) {
		rte_free(qdev->sb_array);
		qdev->sb_array = NULL;
	}
}

int qede_alloc_fp_array(struct qede_dev *qdev)
{
	struct qede_fastpath *fp;
	struct ecore_dev *edev = &qdev->edev;
	int i;

	qdev->fp_array = rte_calloc("fp", QEDE_QUEUE_CNT(qdev),
				    sizeof(*qdev->fp_array),
				    RTE_CACHE_LINE_SIZE);

	if (!qdev->fp_array) {
		DP_ERR(edev, "fp array allocation failed\n");
		return -ENOMEM;
	}

	qdev->sb_array = rte_calloc("sb", QEDE_QUEUE_CNT(qdev),
				    sizeof(*qdev->sb_array),
				    RTE_CACHE_LINE_SIZE);

	if (!qdev->sb_array) {
		DP_ERR(edev, "sb array allocation failed\n");
		rte_free(qdev->fp_array);
		return -ENOMEM;
	}

	return 0;
}

/* This function allocates fast-path status block memory */
static int
qede_alloc_mem_sb(struct qede_dev *qdev, struct ecore_sb_info *sb_info,
		  uint16_t sb_id)
{
	struct ecore_dev *edev = &qdev->edev;
	struct status_block *sb_virt;
	dma_addr_t sb_phys;
	int rc;

	sb_virt = OSAL_DMA_ALLOC_COHERENT(edev, &sb_phys, sizeof(*sb_virt));

	if (!sb_virt) {
		DP_ERR(edev, "Status block allocation failed\n");
		return -ENOMEM;
	}

	rc = qdev->ops->common->sb_init(edev, sb_info,
					sb_virt, sb_phys, sb_id,
					QED_SB_TYPE_L2_QUEUE);
	if (rc) {
		DP_ERR(edev, "Status block initialization failed\n");
		/* TBD: No dma_free_coherent possible */
		return rc;
	}

	return 0;
}

int qede_alloc_fp_resc(struct qede_dev *qdev)
{
	struct ecore_dev *edev = &qdev->edev;
	struct qede_fastpath *fp;
	uint32_t num_sbs;
	int rc, i;

	if (IS_VF(edev))
		ecore_vf_get_num_sbs(ECORE_LEADING_HWFN(edev), &num_sbs);
	else
		num_sbs = (ecore_cxt_get_proto_cid_count
			  (ECORE_LEADING_HWFN(edev), PROTOCOLID_ETH, NULL)) / 2;

	if (num_sbs == 0) {
		DP_ERR(edev, "No status blocks available\n");
		return -EINVAL;
	}

	if (qdev->fp_array)
		qede_free_fp_arrays(qdev);

	rc = qede_alloc_fp_array(qdev);
	if (rc != 0)
		return rc;

	qede_init_fp(qdev);

	for (i = 0; i < QEDE_QUEUE_CNT(qdev); i++) {
		fp = &qdev->fp_array[i];
		if (qede_alloc_mem_sb(qdev, fp->sb_info, i % num_sbs)) {
			qede_free_fp_arrays(qdev);
			return -ENOMEM;
		}
	}

	return 0;
}

void qede_dealloc_fp_resc(struct rte_eth_dev *eth_dev)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);

	qede_free_mem_load(eth_dev);
	qede_free_fp_arrays(qdev);
}

static inline void
qede_update_rx_prod(struct qede_dev *edev, struct qede_rx_queue *rxq)
{
	uint16_t bd_prod = ecore_chain_get_prod_idx(&rxq->rx_bd_ring);
	uint16_t cqe_prod = ecore_chain_get_prod_idx(&rxq->rx_comp_ring);
	struct eth_rx_prod_data rx_prods = { 0 };

	/* Update producers */
	rx_prods.bd_prod = rte_cpu_to_le_16(bd_prod);
	rx_prods.cqe_prod = rte_cpu_to_le_16(cqe_prod);

	/* Make sure that the BD and SGE data is updated before updating the
	 * producers since FW might read the BD/SGE right after the producer
	 * is updated.
	 */
	rte_wmb();

	internal_ram_wr(rxq->hw_rxq_prod_addr, sizeof(rx_prods),
			(uint32_t *)&rx_prods);

	/* mmiowb is needed to synchronize doorbell writes from more than one
	 * processor. It guarantees that the write arrives to the device before
	 * the napi lock is released and another qede_poll is called (possibly
	 * on another CPU). Without this barrier, the next doorbell can bypass
	 * this doorbell. This is applicable to IA64/Altix systems.
	 */
	rte_wmb();

	PMD_RX_LOG(DEBUG, rxq, "bd_prod %u  cqe_prod %u\n", bd_prod, cqe_prod);
}

static inline uint32_t
qede_rxfh_indir_default(uint32_t index, uint32_t n_rx_rings)
{
	return index % n_rx_rings;
}

static void qede_prandom_bytes(uint32_t *buff, size_t bytes)
{
	unsigned int i;

	srand((unsigned int)time(NULL));

	for (i = 0; i < ECORE_RSS_KEY_SIZE; i++)
		buff[i] = rand();
}

static bool
qede_check_vport_rss_enable(struct rte_eth_dev *eth_dev,
			    struct qed_update_vport_rss_params *rss_params)
{
	struct rte_eth_rss_conf rss_conf;
	enum rte_eth_rx_mq_mode mode = eth_dev->data->dev_conf.rxmode.mq_mode;
	struct qede_dev *qdev = eth_dev->data->dev_private;
	struct ecore_dev *edev = &qdev->edev;
	uint8_t rss_caps;
	unsigned int i;
	uint64_t hf;
	uint32_t *key;

	PMD_INIT_FUNC_TRACE(edev);

	rss_conf = eth_dev->data->dev_conf.rx_adv_conf.rss_conf;
	key = (uint32_t *)rss_conf.rss_key;
	hf = rss_conf.rss_hf;

	/* Check if RSS conditions are met.
	 * Note: Even though its meaningless to enable RSS with one queue, it
	 * could be used to produce RSS Hash, so skipping that check.
	 */
	if (!(mode & ETH_MQ_RX_RSS)) {
		DP_INFO(edev, "RSS flag is not set\n");
		return false;
	}

	if (hf == 0) {
		DP_INFO(edev, "Request to disable RSS\n");
		return false;
	}

	memset(rss_params, 0, sizeof(*rss_params));

	for (i = 0; i < ECORE_RSS_IND_TABLE_SIZE; i++)
		rss_params->rss_ind_table[i] = qede_rxfh_indir_default(i,
							QEDE_RSS_COUNT(qdev));

	if (!key)
		qede_prandom_bytes(rss_params->rss_key,
				   sizeof(rss_params->rss_key));
	else
		memcpy(rss_params->rss_key, rss_conf.rss_key,
		       rss_conf.rss_key_len);

	qede_init_rss_caps(&rss_caps, hf);

	rss_params->rss_caps = rss_caps;

	DP_INFO(edev, "RSS conditions are met\n");

	return true;
}

static int qede_start_queues(struct rte_eth_dev *eth_dev, bool clear_stats)
{
	struct qede_dev *qdev = eth_dev->data->dev_private;
	struct ecore_dev *edev = &qdev->edev;
	struct ecore_queue_start_common_params q_params;
	struct qed_update_vport_rss_params *rss_params = &qdev->rss_params;
	struct qed_dev_info *qed_info = &qdev->dev_info.common;
	struct qed_update_vport_params vport_update_params;
	struct qede_tx_queue *txq;
	struct qede_fastpath *fp;
	dma_addr_t p_phys_table;
	int txq_index;
	uint16_t page_cnt;
	int vlan_removal_en = 1;
	int rc, tc, i;

	for_each_queue(i) {
		fp = &qdev->fp_array[i];
		if (fp->type & QEDE_FASTPATH_RX) {
			p_phys_table = ecore_chain_get_pbl_phys(&fp->rxq->
								rx_comp_ring);
			page_cnt = ecore_chain_get_page_cnt(&fp->rxq->
								rx_comp_ring);

			memset(&q_params, 0, sizeof(q_params));
			q_params.queue_id = i;
			q_params.vport_id = 0;
			q_params.sb = fp->sb_info->igu_sb_id;
			q_params.sb_idx = RX_PI;

			ecore_sb_ack(fp->sb_info, IGU_INT_DISABLE, 0);

			rc = qdev->ops->q_rx_start(edev, i, &q_params,
					   fp->rxq->rx_buf_size,
					   fp->rxq->rx_bd_ring.p_phys_addr,
					   p_phys_table,
					   page_cnt,
					   &fp->rxq->hw_rxq_prod_addr);
			if (rc) {
				DP_ERR(edev, "Start rxq #%d failed %d\n",
				       fp->rxq->queue_id, rc);
				return rc;
			}

			fp->rxq->hw_cons_ptr =
					&fp->sb_info->sb_virt->pi_array[RX_PI];

			qede_update_rx_prod(qdev, fp->rxq);
		}

		if (!(fp->type & QEDE_FASTPATH_TX))
			continue;
		for (tc = 0; tc < qdev->num_tc; tc++) {
			txq = fp->txqs[tc];
			txq_index = tc * QEDE_RSS_COUNT(qdev) + i;

			p_phys_table = ecore_chain_get_pbl_phys(&txq->tx_pbl);
			page_cnt = ecore_chain_get_page_cnt(&txq->tx_pbl);

			memset(&q_params, 0, sizeof(q_params));
			q_params.queue_id = txq->queue_id;
			q_params.vport_id = 0;
			q_params.sb = fp->sb_info->igu_sb_id;
			q_params.sb_idx = TX_PI(tc);

			rc = qdev->ops->q_tx_start(edev, i, &q_params,
						   p_phys_table,
						   page_cnt, /* **pp_doorbell */
						   &txq->doorbell_addr);
			if (rc) {
				DP_ERR(edev, "Start txq %u failed %d\n",
				       txq_index, rc);
				return rc;
			}

			txq->hw_cons_ptr =
			    &fp->sb_info->sb_virt->pi_array[TX_PI(tc)];
			SET_FIELD(txq->tx_db.data.params,
				  ETH_DB_DATA_DEST, DB_DEST_XCM);
			SET_FIELD(txq->tx_db.data.params, ETH_DB_DATA_AGG_CMD,
				  DB_AGG_CMD_SET);
			SET_FIELD(txq->tx_db.data.params,
				  ETH_DB_DATA_AGG_VAL_SEL,
				  DQ_XCM_ETH_TX_BD_PROD_CMD);

			txq->tx_db.data.agg_flags = DQ_XCM_ETH_DQ_CF_CMD;
		}
	}

	/* Prepare and send the vport enable */
	memset(&vport_update_params, 0, sizeof(vport_update_params));
	/* Update MTU via vport update */
	vport_update_params.mtu = qdev->mtu;
	vport_update_params.vport_id = 0;
	vport_update_params.update_vport_active_flg = 1;
	vport_update_params.vport_active_flg = 1;

	/* @DPDK */
	if (qed_info->mf_mode == MF_NPAR && qed_info->tx_switching) {
		/* TBD: Check SRIOV enabled for VF */
		vport_update_params.update_tx_switching_flg = 1;
		vport_update_params.tx_switching_flg = 1;
	}

	if (qede_check_vport_rss_enable(eth_dev, rss_params)) {
		vport_update_params.update_rss_flg = 1;
		qdev->rss_enabled = 1;
	} else {
		qdev->rss_enabled = 0;
	}

	rte_memcpy(&vport_update_params.rss_params, rss_params,
	       sizeof(*rss_params));

	rc = qdev->ops->vport_update(edev, &vport_update_params);
	if (rc) {
		DP_ERR(edev, "Update V-PORT failed %d\n", rc);
		return rc;
	}

	return 0;
}

#ifdef ENC_SUPPORTED
static bool qede_tunn_exist(uint16_t flag)
{
	return !!((PARSING_AND_ERR_FLAGS_TUNNELEXIST_MASK <<
		    PARSING_AND_ERR_FLAGS_TUNNELEXIST_SHIFT) & flag);
}

static inline uint8_t qede_check_tunn_csum(uint16_t flag)
{
	uint8_t tcsum = 0;
	uint16_t csum_flag = 0;

	if ((PARSING_AND_ERR_FLAGS_TUNNELL4CHKSMWASCALCULATED_MASK <<
	     PARSING_AND_ERR_FLAGS_TUNNELL4CHKSMWASCALCULATED_SHIFT) & flag)
		csum_flag |= PARSING_AND_ERR_FLAGS_TUNNELL4CHKSMERROR_MASK <<
		    PARSING_AND_ERR_FLAGS_TUNNELL4CHKSMERROR_SHIFT;

	if ((PARSING_AND_ERR_FLAGS_L4CHKSMWASCALCULATED_MASK <<
	     PARSING_AND_ERR_FLAGS_L4CHKSMWASCALCULATED_SHIFT) & flag) {
		csum_flag |= PARSING_AND_ERR_FLAGS_L4CHKSMERROR_MASK <<
		    PARSING_AND_ERR_FLAGS_L4CHKSMERROR_SHIFT;
		tcsum = QEDE_TUNN_CSUM_UNNECESSARY;
	}

	csum_flag |= PARSING_AND_ERR_FLAGS_TUNNELIPHDRERROR_MASK <<
	    PARSING_AND_ERR_FLAGS_TUNNELIPHDRERROR_SHIFT |
	    PARSING_AND_ERR_FLAGS_IPHDRERROR_MASK <<
	    PARSING_AND_ERR_FLAGS_IPHDRERROR_SHIFT;

	if (csum_flag & flag)
		return QEDE_CSUM_ERROR;

	return QEDE_CSUM_UNNECESSARY | tcsum;
}
#else
static inline uint8_t qede_tunn_exist(uint16_t flag)
{
	return 0;
}

static inline uint8_t qede_check_tunn_csum(uint16_t flag)
{
	return 0;
}
#endif

static inline uint8_t qede_check_notunn_csum(uint16_t flag)
{
	uint8_t csum = 0;
	uint16_t csum_flag = 0;

	if ((PARSING_AND_ERR_FLAGS_L4CHKSMWASCALCULATED_MASK <<
	     PARSING_AND_ERR_FLAGS_L4CHKSMWASCALCULATED_SHIFT) & flag) {
		csum_flag |= PARSING_AND_ERR_FLAGS_L4CHKSMERROR_MASK <<
		    PARSING_AND_ERR_FLAGS_L4CHKSMERROR_SHIFT;
		csum = QEDE_CSUM_UNNECESSARY;
	}

	csum_flag |= PARSING_AND_ERR_FLAGS_IPHDRERROR_MASK <<
	    PARSING_AND_ERR_FLAGS_IPHDRERROR_SHIFT;

	if (csum_flag & flag)
		return QEDE_CSUM_ERROR;

	return csum;
}

static inline uint8_t qede_check_csum(uint16_t flag)
{
	if (likely(!qede_tunn_exist(flag)))
		return qede_check_notunn_csum(flag);
	else
		return qede_check_tunn_csum(flag);
}

static inline void qede_rx_bd_ring_consume(struct qede_rx_queue *rxq)
{
	ecore_chain_consume(&rxq->rx_bd_ring);
	rxq->sw_rx_cons++;
}

static inline void
qede_reuse_page(struct qede_dev *qdev,
		struct qede_rx_queue *rxq, struct qede_rx_entry *curr_cons)
{
	struct eth_rx_bd *rx_bd_prod = ecore_chain_produce(&rxq->rx_bd_ring);
	uint16_t idx = rxq->sw_rx_cons & NUM_RX_BDS(rxq);
	struct qede_rx_entry *curr_prod;
	dma_addr_t new_mapping;

	curr_prod = &rxq->sw_rx_ring[idx];
	*curr_prod = *curr_cons;

	new_mapping = rte_mbuf_data_dma_addr_default(curr_prod->mbuf) +
		      curr_prod->page_offset;

	rx_bd_prod->addr.hi = rte_cpu_to_le_32(U64_HI(new_mapping));
	rx_bd_prod->addr.lo = rte_cpu_to_le_32(U64_LO(new_mapping));

	rxq->sw_rx_prod++;
}

static inline void
qede_recycle_rx_bd_ring(struct qede_rx_queue *rxq,
			struct qede_dev *qdev, uint8_t count)
{
	struct qede_rx_entry *curr_cons;

	for (; count > 0; count--) {
		curr_cons = &rxq->sw_rx_ring[rxq->sw_rx_cons & NUM_RX_BDS(rxq)];
		qede_reuse_page(qdev, rxq, curr_cons);
		qede_rx_bd_ring_consume(rxq);
	}
}

static inline uint32_t qede_rx_cqe_to_pkt_type(uint16_t flags)
{
	uint32_t p_type;
	/* TBD - L4 indications needed ? */
	uint16_t protocol = ((PARSING_AND_ERR_FLAGS_L3TYPE_MASK <<
			      PARSING_AND_ERR_FLAGS_L3TYPE_SHIFT) & flags);

	/* protocol = 3 means LLC/SNAP over Ethernet */
	if (unlikely(protocol == 0 || protocol == 3))
		p_type = RTE_PTYPE_UNKNOWN;
	else if (protocol == 1)
		p_type = RTE_PTYPE_L3_IPV4;
	else if (protocol == 2)
		p_type = RTE_PTYPE_L3_IPV6;

	return RTE_PTYPE_L2_ETHER | p_type;
}

int qede_process_sg_pkts(void *p_rxq,  struct rte_mbuf *rx_mb,
			 int num_segs, uint16_t pkt_len)
{
	struct qede_rx_queue *rxq = p_rxq;
	struct qede_dev *qdev = rxq->qdev;
	struct ecore_dev *edev = &qdev->edev;
	uint16_t sw_rx_index, cur_size;

	register struct rte_mbuf *seg1 = NULL;
	register struct rte_mbuf *seg2 = NULL;

	seg1 = rx_mb;
	while (num_segs) {
		cur_size = pkt_len > rxq->rx_buf_size ?
				rxq->rx_buf_size : pkt_len;
		if (!cur_size) {
			PMD_RX_LOG(DEBUG, rxq,
				   "SG packet, len and num BD mismatch\n");
			qede_recycle_rx_bd_ring(rxq, qdev, num_segs);
			return -EINVAL;
		}

		if (qede_alloc_rx_buffer(rxq)) {
			uint8_t index;

			PMD_RX_LOG(DEBUG, rxq, "Buffer allocation failed\n");
			index = rxq->port_id;
			rte_eth_devices[index].data->rx_mbuf_alloc_failed++;
			rxq->rx_alloc_errors++;
			return -ENOMEM;
		}

		sw_rx_index = rxq->sw_rx_cons & NUM_RX_BDS(rxq);
		seg2 = rxq->sw_rx_ring[sw_rx_index].mbuf;
		qede_rx_bd_ring_consume(rxq);
		pkt_len -= cur_size;
		seg2->data_len = cur_size;
		seg1->next = seg2;
		seg1 = seg1->next;
		num_segs--;
		rxq->rx_segs++;
		continue;
	}
	seg1 = NULL;

	if (pkt_len)
		PMD_RX_LOG(DEBUG, rxq,
			   "Mapped all BDs of jumbo, but still have %d bytes\n",
			   pkt_len);

	return ECORE_SUCCESS;
}

uint16_t
qede_recv_pkts(void *p_rxq, struct rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	struct qede_rx_queue *rxq = p_rxq;
	struct qede_dev *qdev = rxq->qdev;
	struct ecore_dev *edev = &qdev->edev;
	struct qede_fastpath *fp = &qdev->fp_array[rxq->queue_id];
	uint16_t hw_comp_cons, sw_comp_cons, sw_rx_index;
	uint16_t rx_pkt = 0;
	union eth_rx_cqe *cqe;
	struct eth_fast_path_rx_reg_cqe *fp_cqe;
	register struct rte_mbuf *rx_mb = NULL;
	register struct rte_mbuf *seg1 = NULL;
	enum eth_rx_cqe_type cqe_type;
	uint16_t len, pad, preload_idx, pkt_len, parse_flag;
	uint8_t csum_flag, num_segs;
	enum rss_hash_type htype;
	int ret;

	hw_comp_cons = rte_le_to_cpu_16(*rxq->hw_cons_ptr);
	sw_comp_cons = ecore_chain_get_cons_idx(&rxq->rx_comp_ring);

	rte_rmb();

	if (hw_comp_cons == sw_comp_cons)
		return 0;

	while (sw_comp_cons != hw_comp_cons) {
		/* Get the CQE from the completion ring */
		cqe =
		    (union eth_rx_cqe *)ecore_chain_consume(&rxq->rx_comp_ring);
		cqe_type = cqe->fast_path_regular.type;

		if (unlikely(cqe_type == ETH_RX_CQE_TYPE_SLOW_PATH)) {
			PMD_RX_LOG(DEBUG, rxq, "Got a slowath CQE\n");

			qdev->ops->eth_cqe_completion(edev, fp->id,
				(struct eth_slow_path_rx_cqe *)cqe);
			goto next_cqe;
		}

		/* Get the data from the SW ring */
		sw_rx_index = rxq->sw_rx_cons & NUM_RX_BDS(rxq);
		rx_mb = rxq->sw_rx_ring[sw_rx_index].mbuf;
		assert(rx_mb != NULL);

		/* non GRO */
		fp_cqe = &cqe->fast_path_regular;

		len = rte_le_to_cpu_16(fp_cqe->len_on_first_bd);
		pad = fp_cqe->placement_offset;
		assert((len + pad) <= rx_mb->buf_len);

		PMD_RX_LOG(DEBUG, rxq,
			   "CQE type = 0x%x, flags = 0x%x, vlan = 0x%x"
			   " len = %u, parsing_flags = %d\n",
			   cqe_type, fp_cqe->bitfields,
			   rte_le_to_cpu_16(fp_cqe->vlan_tag),
			   len, rte_le_to_cpu_16(fp_cqe->pars_flags.flags));

		/* If this is an error packet then drop it */
		parse_flag =
		    rte_le_to_cpu_16(cqe->fast_path_regular.pars_flags.flags);
		csum_flag = qede_check_csum(parse_flag);
		if (unlikely(csum_flag == QEDE_CSUM_ERROR)) {
			PMD_RX_LOG(ERR, rxq,
				   "CQE in CONS = %u has error, flags = 0x%x "
				   "dropping incoming packet\n",
				   sw_comp_cons, parse_flag);
			rxq->rx_hw_errors++;
			qede_recycle_rx_bd_ring(rxq, qdev, fp_cqe->bd_num);
			goto next_cqe;
		}

		if (unlikely(qede_alloc_rx_buffer(rxq) != 0)) {
			PMD_RX_LOG(ERR, rxq,
				   "New buffer allocation failed,"
				   "dropping incoming packet\n");
			qede_recycle_rx_bd_ring(rxq, qdev, fp_cqe->bd_num);
			rte_eth_devices[rxq->port_id].
			    data->rx_mbuf_alloc_failed++;
			rxq->rx_alloc_errors++;
			break;
		}

		qede_rx_bd_ring_consume(rxq);

		if (fp_cqe->bd_num > 1) {
			pkt_len = rte_le_to_cpu_16(fp_cqe->pkt_len);
			num_segs = fp_cqe->bd_num - 1;

			rxq->rx_segs++;

			pkt_len -= len;
			seg1 = rx_mb;
			ret = qede_process_sg_pkts(p_rxq, seg1, num_segs,
						   pkt_len);
			if (ret != ECORE_SUCCESS) {
				qede_recycle_rx_bd_ring(rxq, qdev,
							fp_cqe->bd_num);
				goto next_cqe;
			}
		}

		/* Prefetch next mbuf while processing current one. */
		preload_idx = rxq->sw_rx_cons & NUM_RX_BDS(rxq);
		rte_prefetch0(rxq->sw_rx_ring[preload_idx].mbuf);

		/* Update MBUF fields */
		rx_mb->ol_flags = 0;
		rx_mb->data_off = pad + RTE_PKTMBUF_HEADROOM;
		rx_mb->nb_segs = fp_cqe->bd_num;
		rx_mb->data_len = len;
		rx_mb->pkt_len = fp_cqe->pkt_len;
		rx_mb->port = rxq->port_id;
		rx_mb->packet_type = qede_rx_cqe_to_pkt_type(parse_flag);

		htype = (uint8_t)GET_FIELD(fp_cqe->bitfields,
				ETH_FAST_PATH_RX_REG_CQE_RSS_HASH_TYPE);
		if (qdev->rss_enabled && htype) {
			rx_mb->ol_flags |= PKT_RX_RSS_HASH;
			rx_mb->hash.rss = rte_le_to_cpu_32(fp_cqe->rss_hash);
			PMD_RX_LOG(DEBUG, rxq, "Hash result 0x%x\n",
				   rx_mb->hash.rss);
		}

		rte_prefetch1(rte_pktmbuf_mtod(rx_mb, void *));

		if (CQE_HAS_VLAN(parse_flag)) {
			rx_mb->vlan_tci = rte_le_to_cpu_16(fp_cqe->vlan_tag);
			rx_mb->ol_flags |= PKT_RX_VLAN_PKT;
		}

		if (CQE_HAS_OUTER_VLAN(parse_flag)) {
			/* FW does not provide indication of Outer VLAN tag,
			 * which is always stripped, so vlan_tci_outer is set
			 * to 0. Here vlan_tag represents inner VLAN tag.
			 */
			rx_mb->vlan_tci = rte_le_to_cpu_16(fp_cqe->vlan_tag);
			rx_mb->ol_flags |= PKT_RX_QINQ_PKT;
			rx_mb->vlan_tci_outer = 0;
		}

		rx_pkts[rx_pkt] = rx_mb;
		rx_pkt++;
next_cqe:
		ecore_chain_recycle_consumed(&rxq->rx_comp_ring);
		sw_comp_cons = ecore_chain_get_cons_idx(&rxq->rx_comp_ring);
		if (rx_pkt == nb_pkts) {
			PMD_RX_LOG(DEBUG, rxq,
				   "Budget reached nb_pkts=%u received=%u\n",
				   rx_pkt, nb_pkts);
			break;
		}
	}

	qede_update_rx_prod(qdev, rxq);

	rxq->rcv_pkts += rx_pkt;

	PMD_RX_LOG(DEBUG, rxq, "rx_pkts=%u core=%d\n", rx_pkt, rte_lcore_id());

	return rx_pkt;
}

static inline int
qede_free_tx_pkt(struct ecore_dev *edev, struct qede_tx_queue *txq)
{
	uint16_t nb_segs, idx = TX_CONS(txq);
	struct eth_tx_bd *tx_data_bd;
	struct rte_mbuf *mbuf = txq->sw_tx_ring[idx].mbuf;

	if (unlikely(!mbuf)) {
		PMD_TX_LOG(ERR, txq, "null mbuf\n");
		PMD_TX_LOG(ERR, txq,
			   "tx_desc %u tx_avail %u tx_cons %u tx_prod %u\n",
			   txq->nb_tx_desc, txq->nb_tx_avail, idx,
			   TX_PROD(txq));
		return -1;
	}

	nb_segs = mbuf->nb_segs;
	while (nb_segs) {
		/* It's like consuming rxbuf in recv() */
		ecore_chain_consume(&txq->tx_pbl);
		txq->nb_tx_avail++;
		nb_segs--;
	}
	rte_pktmbuf_free(mbuf);
	txq->sw_tx_ring[idx].mbuf = NULL;

	return 0;
}

static inline uint16_t
qede_process_tx_compl(struct ecore_dev *edev, struct qede_tx_queue *txq)
{
	uint16_t tx_compl = 0;
	uint16_t hw_bd_cons;

	hw_bd_cons = rte_le_to_cpu_16(*txq->hw_cons_ptr);
	rte_compiler_barrier();

	while (hw_bd_cons != ecore_chain_get_cons_idx(&txq->tx_pbl)) {
		if (qede_free_tx_pkt(edev, txq)) {
			PMD_TX_LOG(ERR, txq,
				   "hw_bd_cons = %u, chain_cons = %u\n",
				   hw_bd_cons,
				   ecore_chain_get_cons_idx(&txq->tx_pbl));
			break;
		}
		txq->sw_tx_cons++;	/* Making TXD available */
		tx_compl++;
	}

	PMD_TX_LOG(DEBUG, txq, "Tx compl %u sw_tx_cons %u avail %u\n",
		   tx_compl, txq->sw_tx_cons, txq->nb_tx_avail);
	return tx_compl;
}

/* Populate scatter gather buffer descriptor fields */
static inline uint16_t qede_encode_sg_bd(struct qede_tx_queue *p_txq,
					 struct rte_mbuf *m_seg,
					 uint16_t count,
					 struct eth_tx_1st_bd *bd1)
{
	struct qede_tx_queue *txq = p_txq;
	struct eth_tx_2nd_bd *bd2 = NULL;
	struct eth_tx_3rd_bd *bd3 = NULL;
	struct eth_tx_bd *tx_bd = NULL;
	uint16_t nb_segs = count;
	dma_addr_t mapping;

	/* Check for scattered buffers */
	while (m_seg) {
		if (nb_segs == 1) {
			bd2 = (struct eth_tx_2nd_bd *)
				ecore_chain_produce(&txq->tx_pbl);
			memset(bd2, 0, sizeof(*bd2));
			mapping = rte_mbuf_data_dma_addr(m_seg);
			bd2->addr.hi = rte_cpu_to_le_32(U64_HI(mapping));
			bd2->addr.lo = rte_cpu_to_le_32(U64_LO(mapping));
			bd2->nbytes = rte_cpu_to_le_16(m_seg->data_len);
		} else if (nb_segs == 2) {
			bd3 = (struct eth_tx_3rd_bd *)
				ecore_chain_produce(&txq->tx_pbl);
			memset(bd3, 0, sizeof(*bd3));
			mapping = rte_mbuf_data_dma_addr(m_seg);
			bd3->addr.hi = rte_cpu_to_le_32(U64_HI(mapping));
			bd3->addr.lo = rte_cpu_to_le_32(U64_LO(mapping));
			bd3->nbytes = rte_cpu_to_le_16(m_seg->data_len);
		} else {
			tx_bd = (struct eth_tx_bd *)
				ecore_chain_produce(&txq->tx_pbl);
			memset(tx_bd, 0, sizeof(*tx_bd));
			mapping = rte_mbuf_data_dma_addr(m_seg);
			tx_bd->addr.hi = rte_cpu_to_le_32(U64_HI(mapping));
			tx_bd->addr.lo = rte_cpu_to_le_32(U64_LO(mapping));
			tx_bd->nbytes = rte_cpu_to_le_16(m_seg->data_len);
		}
		nb_segs++;
		bd1->data.nbds = nb_segs;
		m_seg = m_seg->next;
	}

	/* Return total scattered buffers */
	return nb_segs;
}

uint16_t
qede_xmit_pkts(void *p_txq, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	struct qede_tx_queue *txq = p_txq;
	struct qede_dev *qdev = txq->qdev;
	struct ecore_dev *edev = &qdev->edev;
	struct qede_fastpath *fp;
	struct eth_tx_1st_bd *bd1;
	struct rte_mbuf *m_seg = NULL;
	uint16_t nb_tx_pkts;
	uint16_t nb_pkt_sent = 0;
	uint16_t bd_prod;
	uint16_t idx;
	uint16_t tx_count;
	uint16_t nb_segs = 0;

	fp = &qdev->fp_array[QEDE_RSS_COUNT(qdev) + txq->queue_id];

	if (unlikely(txq->nb_tx_avail < txq->tx_free_thresh)) {
		PMD_TX_LOG(DEBUG, txq, "send=%u avail=%u free_thresh=%u\n",
			   nb_pkts, txq->nb_tx_avail, txq->tx_free_thresh);
		(void)qede_process_tx_compl(edev, txq);
	}

	nb_tx_pkts = RTE_MIN(nb_pkts, (txq->nb_tx_avail /
			ETH_TX_MAX_BDS_PER_NON_LSO_PACKET));
	if (unlikely(nb_tx_pkts == 0)) {
		PMD_TX_LOG(DEBUG, txq, "Out of BDs nb_pkts=%u avail=%u\n",
			   nb_pkts, txq->nb_tx_avail);
		return 0;
	}

	tx_count = nb_tx_pkts;
	while (nb_tx_pkts--) {
		/* Fill the entry in the SW ring and the BDs in the FW ring */
		idx = TX_PROD(txq);
		struct rte_mbuf *mbuf = *tx_pkts++;

		txq->sw_tx_ring[idx].mbuf = mbuf;
		bd1 = (struct eth_tx_1st_bd *)ecore_chain_produce(&txq->tx_pbl);
		/* Zero init struct fields */
		bd1->data.bd_flags.bitfields = 0;
		bd1->data.bitfields = 0;

		bd1->data.bd_flags.bitfields =
			1 << ETH_TX_1ST_BD_FLAGS_START_BD_SHIFT;
		/* Map MBUF linear data for DMA and set in the first BD */
		QEDE_BD_SET_ADDR_LEN(bd1, rte_mbuf_data_dma_addr(mbuf),
				     mbuf->pkt_len);

		/* Descriptor based VLAN insertion */
		if (mbuf->ol_flags & (PKT_TX_VLAN_PKT | PKT_TX_QINQ_PKT)) {
			bd1->data.vlan = rte_cpu_to_le_16(mbuf->vlan_tci);
			bd1->data.bd_flags.bitfields |=
			    1 << ETH_TX_1ST_BD_FLAGS_VLAN_INSERTION_SHIFT;
		}

		/* Offload the IP checksum in the hardware */
		if (mbuf->ol_flags & PKT_TX_IP_CKSUM) {
			bd1->data.bd_flags.bitfields |=
			    1 << ETH_TX_1ST_BD_FLAGS_IP_CSUM_SHIFT;
		}

		/* L4 checksum offload (tcp or udp) */
		if (mbuf->ol_flags & (PKT_TX_TCP_CKSUM | PKT_TX_UDP_CKSUM)) {
			bd1->data.bd_flags.bitfields |=
			    1 << ETH_TX_1ST_BD_FLAGS_L4_CSUM_SHIFT;
			/* IPv6 + extn. -> later */
		}

		/* Handle fragmented MBUF */
		m_seg = mbuf->next;
		nb_segs++;
		bd1->data.nbds = nb_segs;
		/* Encode scatter gather buffer descriptors if required */
		nb_segs = qede_encode_sg_bd(txq, m_seg, nb_segs, bd1);
		txq->nb_tx_avail = txq->nb_tx_avail - nb_segs;
		nb_segs = 0;
		txq->sw_tx_prod++;
		rte_prefetch0(txq->sw_tx_ring[TX_PROD(txq)].mbuf);
		bd_prod =
		    rte_cpu_to_le_16(ecore_chain_get_prod_idx(&txq->tx_pbl));
		nb_pkt_sent++;
		txq->xmit_pkts++;
	}

	/* Write value of prod idx into bd_prod */
	txq->tx_db.data.bd_prod = bd_prod;
	rte_wmb();
	rte_compiler_barrier();
	DIRECT_REG_WR(edev, txq->doorbell_addr, txq->tx_db.raw);
	rte_wmb();

	/* Check again for Tx completions */
	(void)qede_process_tx_compl(edev, txq);

	PMD_TX_LOG(DEBUG, txq, "to_send=%u can_send=%u sent=%u core=%d\n",
		   nb_pkts, tx_count, nb_pkt_sent, rte_lcore_id());

	return nb_pkt_sent;
}

static void qede_init_fp_queue(struct rte_eth_dev *eth_dev)
{
	struct qede_dev *qdev = eth_dev->data->dev_private;
	struct qede_fastpath *fp;
	uint8_t i, rss_id, txq_index, tc;
	int rxq = 0, txq = 0;

	for_each_queue(i) {
		fp = &qdev->fp_array[i];
		if (fp->type & QEDE_FASTPATH_RX) {
			fp->rxq = eth_dev->data->rx_queues[i];
			fp->rxq->queue_id = rxq++;
		}

		if (fp->type & QEDE_FASTPATH_TX) {
			for (tc = 0; tc < qdev->num_tc; tc++) {
				txq_index = tc * QEDE_TSS_COUNT(qdev) + txq;
				fp->txqs[tc] =
					eth_dev->data->tx_queues[txq_index];
				fp->txqs[tc]->queue_id = txq_index;
			}
			txq++;
		}
	}
}

int qede_dev_start(struct rte_eth_dev *eth_dev)
{
	struct qede_dev *qdev = eth_dev->data->dev_private;
	struct ecore_dev *edev = &qdev->edev;
	struct qed_link_output link_output;
	struct qede_fastpath *fp;
	int rc, i;

	DP_INFO(edev, "Device state is %d\n", qdev->state);

	if (qdev->state == QEDE_DEV_START) {
		DP_INFO(edev, "Port is already started\n");
		return 0;
	}

	if (qdev->state == QEDE_DEV_CONFIG)
		qede_init_fp_queue(eth_dev);

	rc = qede_start_queues(eth_dev, true);
	if (rc) {
		DP_ERR(edev, "Failed to start queues\n");
		/* TBD: free */
		return rc;
	}

	/* Bring-up the link */
	qede_dev_set_link_state(eth_dev, true);

	/* Reset ring */
	if (qede_reset_fp_rings(qdev))
		return -ENOMEM;

	/* Start/resume traffic */
	qdev->ops->fastpath_start(edev);

	qdev->state = QEDE_DEV_START;

	DP_INFO(edev, "dev_state is QEDE_DEV_START\n");

	return 0;
}

static int qede_drain_txq(struct qede_dev *qdev,
			  struct qede_tx_queue *txq, bool allow_drain)
{
	struct ecore_dev *edev = &qdev->edev;
	int rc, cnt = 1000;

	while (txq->sw_tx_cons != txq->sw_tx_prod) {
		qede_process_tx_compl(edev, txq);
		if (!cnt) {
			if (allow_drain) {
				DP_NOTICE(edev, false,
					  "Tx queue[%u] is stuck,"
					  "requesting MCP to drain\n",
					  txq->queue_id);
				rc = qdev->ops->common->drain(edev);
				if (rc)
					return rc;
				return qede_drain_txq(qdev, txq, false);
			}

			DP_NOTICE(edev, false,
				  "Timeout waiting for tx queue[%d]:"
				  "PROD=%d, CONS=%d\n",
				  txq->queue_id, txq->sw_tx_prod,
				  txq->sw_tx_cons);
			return -ENODEV;
		}
		cnt--;
		DELAY(1000);
		rte_compiler_barrier();
	}

	/* FW finished processing, wait for HW to transmit all tx packets */
	DELAY(2000);

	return 0;
}

static int qede_stop_queues(struct qede_dev *qdev)
{
	struct qed_update_vport_params vport_update_params;
	struct ecore_dev *edev = &qdev->edev;
	int rc, tc, i;

	/* Disable the vport */
	memset(&vport_update_params, 0, sizeof(vport_update_params));
	vport_update_params.vport_id = 0;
	vport_update_params.update_vport_active_flg = 1;
	vport_update_params.vport_active_flg = 0;
	vport_update_params.update_rss_flg = 0;

	DP_INFO(edev, "Deactivate vport\n");

	rc = qdev->ops->vport_update(edev, &vport_update_params);
	if (rc) {
		DP_ERR(edev, "Failed to update vport\n");
		return rc;
	}

	DP_INFO(edev, "Flushing tx queues\n");

	/* Flush Tx queues. If needed, request drain from MCP */
	for_each_queue(i) {
		struct qede_fastpath *fp = &qdev->fp_array[i];

		if (fp->type & QEDE_FASTPATH_TX) {
			for (tc = 0; tc < qdev->num_tc; tc++) {
				struct qede_tx_queue *txq = fp->txqs[tc];

				rc = qede_drain_txq(qdev, txq, true);
				if (rc)
					return rc;
			}
		}
	}

	/* Stop all Queues in reverse order */
	for (i = QEDE_QUEUE_CNT(qdev) - 1; i >= 0; i--) {
		struct qed_stop_rxq_params rx_params;

		/* Stop the Tx Queue(s) */
		if (qdev->fp_array[i].type & QEDE_FASTPATH_TX) {
			for (tc = 0; tc < qdev->num_tc; tc++) {
				struct qed_stop_txq_params tx_params;
				u8 val;

				tx_params.rss_id = i;
				val = qdev->fp_array[i].txqs[tc]->queue_id;
				tx_params.tx_queue_id = val;

				DP_INFO(edev, "Stopping tx queues\n");
				rc = qdev->ops->q_tx_stop(edev, &tx_params);
				if (rc) {
					DP_ERR(edev, "Failed to stop TXQ #%d\n",
					       tx_params.tx_queue_id);
					return rc;
				}
			}
		}

		/* Stop the Rx Queue */
		if (qdev->fp_array[i].type & QEDE_FASTPATH_RX) {
			memset(&rx_params, 0, sizeof(rx_params));
			rx_params.rss_id = i;
			rx_params.rx_queue_id = qdev->fp_array[i].rxq->queue_id;
			rx_params.eq_completion_only = 1;

			DP_INFO(edev, "Stopping rx queues\n");

			rc = qdev->ops->q_rx_stop(edev, &rx_params);
			if (rc) {
				DP_ERR(edev, "Failed to stop RXQ #%d\n", i);
				return rc;
			}
		}
	}

	return 0;
}

int qede_reset_fp_rings(struct qede_dev *qdev)
{
	struct qede_fastpath *fp;
	struct qede_tx_queue *txq;
	uint8_t tc;
	uint16_t id, i;

	for_each_queue(id) {
		fp = &qdev->fp_array[id];

		if (fp->type & QEDE_FASTPATH_RX) {
			DP_INFO(&qdev->edev,
				"Reset FP chain for RSS %u\n", id);
			qede_rx_queue_release_mbufs(fp->rxq);
			ecore_chain_reset(&fp->rxq->rx_bd_ring);
			ecore_chain_reset(&fp->rxq->rx_comp_ring);
			fp->rxq->sw_rx_prod = 0;
			fp->rxq->sw_rx_cons = 0;
			*fp->rxq->hw_cons_ptr = 0;
			for (i = 0; i < fp->rxq->nb_rx_desc; i++) {
				if (qede_alloc_rx_buffer(fp->rxq)) {
					DP_ERR(&qdev->edev,
					       "RX buffer allocation failed\n");
					return -ENOMEM;
				}
			}
		}
		if (fp->type & QEDE_FASTPATH_TX) {
			for (tc = 0; tc < qdev->num_tc; tc++) {
				txq = fp->txqs[tc];
				qede_tx_queue_release_mbufs(txq);
				ecore_chain_reset(&txq->tx_pbl);
				txq->sw_tx_cons = 0;
				txq->sw_tx_prod = 0;
				*txq->hw_cons_ptr = 0;
			}
		}
	}

	return 0;
}

/* This function frees all memory of a single fp */
void qede_free_mem_load(struct rte_eth_dev *eth_dev)
{
	struct qede_dev *qdev = QEDE_INIT_QDEV(eth_dev);
	struct qede_fastpath *fp;
	uint16_t txq_idx;
	uint8_t id;
	uint8_t tc;

	for_each_queue(id) {
		fp = &qdev->fp_array[id];
		if (fp->type & QEDE_FASTPATH_RX) {
			qede_rx_queue_release(fp->rxq);
			eth_dev->data->rx_queues[id] = NULL;
		} else {
			for (tc = 0; tc < qdev->num_tc; tc++) {
				txq_idx = fp->txqs[tc]->queue_id;
				qede_tx_queue_release(fp->txqs[tc]);
				eth_dev->data->tx_queues[txq_idx] = NULL;
			}
		}
	}
}

void qede_dev_stop(struct rte_eth_dev *eth_dev)
{
	struct qede_dev *qdev = eth_dev->data->dev_private;
	struct ecore_dev *edev = &qdev->edev;

	DP_INFO(edev, "port %u\n", eth_dev->data->port_id);

	if (qdev->state != QEDE_DEV_START) {
		DP_INFO(edev, "Device not yet started\n");
		return;
	}

	if (qede_stop_queues(qdev))
		DP_ERR(edev, "Didn't succeed to close queues\n");

	DP_INFO(edev, "Stopped queues\n");

	qdev->ops->fastpath_stop(edev);

	/* Bring the link down */
	qede_dev_set_link_state(eth_dev, false);

	qdev->state = QEDE_DEV_STOP;

	DP_INFO(edev, "dev_state is QEDE_DEV_STOP\n");
}
