// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2019 Google, Inc.
 */

#include <linux/cpumask.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <net/sch_generic.h>
#include "gve.h"
#include "gve_adminq.h"
#include "gve_register.h"

#define GVE_DEFAULT_RX_COPYBREAK	(256)

#define DEFAULT_MSG_LEVEL	(NETIF_MSG_DRV | NETIF_MSG_LINK)
#define GVE_VERSION		"1.2.0"
#define GVE_VERSION_PREFIX	"GVE-"

const char gve_version_str[] = GVE_VERSION;
static const char gve_version_prefix[] = GVE_VERSION_PREFIX;

static void gve_get_stats(struct net_device *dev, struct rtnl_link_stats64 *s)
{
	struct gve_priv *priv = netdev_priv(dev);
	unsigned int start;
	int ring;

	if (priv->rx) {
		for (ring = 0; ring < priv->rx_cfg.num_queues; ring++) {
			do {
				start =
				  u64_stats_fetch_begin(&priv->rx[ring].statss);
				s->rx_packets += priv->rx[ring].rpackets;
				s->rx_bytes += priv->rx[ring].rbytes;
			} while (u64_stats_fetch_retry(&priv->rx[ring].statss,
						       start));
		}
	}
	if (priv->tx) {
		for (ring = 0; ring < priv->tx_cfg.num_queues; ring++) {
			do {
				start =
				  u64_stats_fetch_begin(&priv->tx[ring].statss);
				s->tx_packets += priv->tx[ring].pkt_done;
				s->tx_bytes += priv->tx[ring].bytes_done;
			} while (u64_stats_fetch_retry(&priv->tx[ring].statss,
						       start));
		}
	}
}

static int gve_alloc_counter_array(struct gve_priv *priv)
{
	priv->counter_array =
		dma_alloc_coherent(&priv->pdev->dev,
				   priv->num_event_counters *
				   sizeof(*priv->counter_array),
				   &priv->counter_array_bus, GFP_KERNEL);
	if (!priv->counter_array)
		return -ENOMEM;

	return 0;
}

static void gve_free_counter_array(struct gve_priv *priv)
{
	dma_free_coherent(&priv->pdev->dev,
			  priv->num_event_counters *
			  sizeof(*priv->counter_array),
			  priv->counter_array, priv->counter_array_bus);
	priv->counter_array = NULL;
}

void gve_service_task_schedule(struct gve_priv *priv)
{
	if (!gve_get_probe_in_progress(priv) &&
	    !gve_get_reset_in_progress(priv)) {
		gve_set_do_report_stats(priv);
		queue_work(priv->gve_wq, &priv->service_task);
	}
}

static void gve_service_timer(struct timer_list *t)
{
	struct gve_priv *priv = from_timer(priv, t, service_timer);

	mod_timer(&priv->service_timer,
		  round_jiffies(jiffies +
		  msecs_to_jiffies(priv->service_timer_period)));
	gve_service_task_schedule(priv);
}

static int gve_alloc_stats_report(struct gve_priv *priv)
{
	int tx_stats_num, rx_stats_num;

	tx_stats_num = (GVE_TX_STATS_REPORT_NUM + NIC_TX_STATS_REPORT_NUM) *
		       priv->tx_cfg.num_queues;
	rx_stats_num = (GVE_RX_STATS_REPORT_NUM + NIC_RX_STATS_REPORT_NUM) *
		       priv->rx_cfg.num_queues;
	priv->stats_report_len = sizeof(struct gve_stats_report) +
				 (tx_stats_num + rx_stats_num) *
				 sizeof(struct stats);
	priv->stats_report =
		dma_alloc_coherent(&priv->pdev->dev, priv->stats_report_len,
				   &priv->stats_report_bus, GFP_KERNEL);
	if (!priv->stats_report)
		return -ENOMEM;
	/* Set up timer for periodic task */
	timer_setup(&priv->service_timer, gve_service_timer, 0);
	priv->service_timer_period = GVE_SERVICE_TIMER_PERIOD;
	/* Start the service task timer */
	mod_timer(&priv->service_timer,
		  round_jiffies(jiffies +
		  msecs_to_jiffies(priv->service_timer_period)));
	return 0;
}

static void gve_free_stats_report(struct gve_priv *priv)
{

	del_timer_sync(&priv->service_timer);
	dma_free_coherent(&priv->pdev->dev, priv->stats_report_len,
			  priv->stats_report, priv->stats_report_bus);
	priv->stats_report = NULL;
}

static irqreturn_t gve_mgmnt_intr(int irq, void *arg)
{
	struct gve_priv *priv = arg;

	queue_work(priv->gve_wq, &priv->service_task);
	return IRQ_HANDLED;
}

static irqreturn_t gve_intr(int irq, void *arg)
{
	struct gve_notify_block *block = arg;
	struct gve_priv *priv = block->priv;

	if (block->tx)
		block->tx->last_nic_done_irq = gve_tx_load_event_counter(priv, block->tx);
	iowrite32be(GVE_IRQ_MASK, gve_irq_doorbell(priv, block));
	napi_schedule_irqoff(&block->napi);
	return IRQ_HANDLED;
}

static int gve_napi_poll(struct napi_struct *napi, int budget)
{
	struct gve_notify_block *block;
	__be32 __iomem *irq_doorbell;
	bool reschedule = false;
	struct gve_priv *priv;
	int work_done = 0;

	block = container_of(napi, struct gve_notify_block, napi);
	priv = block->priv;

	if (block->tx)
		reschedule |= gve_tx_poll(block, budget);
	if (block->rx) {
		work_done = gve_rx_poll(block, budget);
		reschedule |= work_done == budget;
	}

	if (reschedule)
		return budget;

	/* Complete processing - don't unmask irq if busy polling is enabled */
	if (likely(napi_complete_done(napi, work_done))) {
		irq_doorbell = gve_irq_doorbell(priv, block);
		iowrite32be(GVE_IRQ_ACK | GVE_IRQ_EVENT, irq_doorbell);

		/* Double check we have no extra work.
		 * Ensure unmask synchronizes with checking for work.
		 */
		smp_mb();
		if(block->tx)
			block->tx->last_nic_done_clear = gve_tx_load_event_counter(priv, block->tx);

		if (block->tx) reschedule |= gve_tx_poll(block, -1);
		if (block->rx) reschedule |= gve_rx_work_pending(block->rx);

		if (reschedule && napi_reschedule(napi))
			iowrite32be(GVE_IRQ_MASK, irq_doorbell);
	}
	return work_done;
}

static int gve_alloc_notify_blocks(struct gve_priv *priv)
{
	int num_vecs_requested = priv->num_ntfy_blks + 1;
	char *name = priv->dev->name;
	unsigned int active_cpus;
	int vecs_enabled;
	int i, j;
	int err;

	priv->msix_vectors = kvzalloc(num_vecs_requested *
				      sizeof(*priv->msix_vectors), GFP_KERNEL);
	if (!priv->msix_vectors)
		return -ENOMEM;
	for (i = 0; i < num_vecs_requested; i++)
		priv->msix_vectors[i].entry = i;
	vecs_enabled = pci_enable_msix_range(priv->pdev, priv->msix_vectors,
					     GVE_MIN_MSIX, num_vecs_requested);
	if (vecs_enabled < 0) {
		dev_err(&priv->pdev->dev, "Could not enable min msix %d/%d\n",
			GVE_MIN_MSIX, vecs_enabled);
		err = vecs_enabled;
		goto abort_with_msix_vectors;
	}
	if (vecs_enabled != num_vecs_requested) {
		int new_num_ntfy_blks = (vecs_enabled - 1) & ~0x1;
		int vecs_per_type = new_num_ntfy_blks / 2;
		int vecs_left = new_num_ntfy_blks % 2;

		priv->num_ntfy_blks = new_num_ntfy_blks;
		priv->tx_cfg.max_queues = min_t(int, priv->tx_cfg.max_queues,
						vecs_per_type);
		priv->rx_cfg.max_queues = min_t(int, priv->rx_cfg.max_queues,
						vecs_per_type + vecs_left);
		dev_err(&priv->pdev->dev,
			"Could not enable desired msix, only enabled %d, adjusting tx max queues to %d, and rx max queues to %d\n",
			vecs_enabled, priv->tx_cfg.max_queues,
			priv->rx_cfg.max_queues);
		if (priv->tx_cfg.num_queues > priv->tx_cfg.max_queues)
			priv->tx_cfg.num_queues = priv->tx_cfg.max_queues;
		if (priv->rx_cfg.num_queues > priv->rx_cfg.max_queues)
			priv->rx_cfg.num_queues = priv->rx_cfg.max_queues;
	}
	/* Half the notification blocks go to TX and half to RX */
	active_cpus = min_t(int, priv->num_ntfy_blks / 2, num_online_cpus());

	/* Setup Management Vector  - the last vector */
	snprintf(priv->mgmt_msix_name, sizeof(priv->mgmt_msix_name), "%s-mgmnt",
		 name);
	err = request_irq(priv->msix_vectors[priv->mgmt_msix_idx].vector,
			  gve_mgmnt_intr, 0, priv->mgmt_msix_name, priv);
	if (err) {
		dev_err(&priv->pdev->dev, "Did not receive management vector.\n");
		goto abort_with_msix_enabled;
	}

	priv->irq_db_indices =
		dma_alloc_coherent(&priv->pdev->dev,
				   priv->num_ntfy_blks *
				   sizeof(*priv->irq_db_indices),
				   &priv->irq_db_indices_bus, GFP_KERNEL);
	if (!priv->irq_db_indices) {
		err = -ENOMEM;
		goto abort_with_mgmt_vector;
	}

	priv->ntfy_blocks = kvzalloc(priv->num_ntfy_blks *
				     sizeof(*priv->ntfy_blocks), GFP_KERNEL);
	if (!priv->ntfy_blocks) {
		err = -ENOMEM;
		goto abort_with_irq_db_indices;
	}

	/* Setup the other blocks - the first n-1 vectors */
	for (i = 0; i < priv->num_ntfy_blks; i++) {
		struct gve_notify_block *block = &priv->ntfy_blocks[i];
		int msix_idx = i;

		snprintf(block->name, sizeof(block->name), "%s-ntfy-block.%d",
			 name, i);
		block->priv = priv;
		err = request_irq(priv->msix_vectors[msix_idx].vector,
				  gve_intr, 0, block->name, block);
		if (err) {
			dev_err(&priv->pdev->dev,
				"Failed to receive msix vector %d\n", i);
			goto abort_with_some_ntfy_blocks;
		}
		irq_set_affinity_hint(priv->msix_vectors[msix_idx].vector,
				      get_cpu_mask(i % active_cpus));
		block->irq_db_index = &priv->irq_db_indices[i].index;
	}
	return 0;
abort_with_some_ntfy_blocks:
	for (j = 0; j < i; j++) {
		struct gve_notify_block *block = &priv->ntfy_blocks[j];
		int msix_idx = j;

		irq_set_affinity_hint(priv->msix_vectors[msix_idx].vector,
				      NULL);
		free_irq(priv->msix_vectors[msix_idx].vector, block);
	}
	kvfree(priv->ntfy_blocks);
	priv->ntfy_blocks = NULL;
abort_with_irq_db_indices:
	dma_free_coherent(&priv->pdev->dev, priv->num_ntfy_blks *
			  sizeof(*priv->irq_db_indices),
			  priv->irq_db_indices, priv->irq_db_indices_bus);
	priv->irq_db_indices = NULL;
abort_with_mgmt_vector:
	free_irq(priv->msix_vectors[priv->mgmt_msix_idx].vector, priv);
abort_with_msix_enabled:
	pci_disable_msix(priv->pdev);
abort_with_msix_vectors:
	kfree(priv->msix_vectors);
	priv->msix_vectors = NULL;
	return err;
}

static void gve_free_notify_blocks(struct gve_priv *priv)
{
	int i;

	/* Free the irqs */
	for (i = 0; i < priv->num_ntfy_blks; i++) {
		struct gve_notify_block *block = &priv->ntfy_blocks[i];
		int msix_idx = i;

		irq_set_affinity_hint(priv->msix_vectors[msix_idx].vector,
				      NULL);
		free_irq(priv->msix_vectors[msix_idx].vector, block);
	}
	kvfree(priv->ntfy_blocks);
	priv->ntfy_blocks = NULL;
	dma_free_coherent(&priv->pdev->dev, priv->num_ntfy_blks *
			  sizeof(*priv->irq_db_indices),
			  priv->irq_db_indices, priv->irq_db_indices_bus);
	priv->irq_db_indices = NULL;
	free_irq(priv->msix_vectors[priv->mgmt_msix_idx].vector, priv);
	pci_disable_msix(priv->pdev);
	kfree(priv->msix_vectors);
	priv->msix_vectors = NULL;
}

static int gve_setup_device_resources(struct gve_priv *priv)
{
	int err;

	err = gve_alloc_counter_array(priv);
	if (err)
		return err;
	err = gve_alloc_notify_blocks(priv);
	if (err)
		goto abort_with_counter;
	err = gve_alloc_stats_report(priv);
	if (err)
		goto abort_with_ntfy_blocks;
	err = gve_adminq_configure_device_resources(priv,
						    priv->counter_array_bus,
						    priv->num_event_counters,
						    priv->irq_db_indices_bus,
						    priv->num_ntfy_blks);
	if (unlikely(err)) {
		dev_err(&priv->pdev->dev,
			"could not setup device_resources: err=%d\n", err);
		err = -ENXIO;
		goto abort_with_stats_report;
	}
	err = gve_adminq_report_stats(priv, priv->stats_report_len,
				      priv->stats_report_bus,
				      GVE_SERVICE_TIMER_PERIOD);
	if (err)
		dev_err(&priv->pdev->dev,
			"Failed to report stats: err=%d\n", err);
	gve_set_device_resources_ok(priv);
	return 0;
abort_with_stats_report:
	gve_free_stats_report(priv);
abort_with_ntfy_blocks:
	gve_free_notify_blocks(priv);
abort_with_counter:
	gve_free_counter_array(priv);
	return err;
}

static void gve_trigger_reset(struct gve_priv *priv);

static void gve_teardown_device_resources(struct gve_priv *priv)
{
	int err;

	/* Tell device its resources are being freed */
	if (gve_get_device_resources_ok(priv)) {
		/* detach the stats report */
		err = gve_adminq_report_stats(priv, 0, 0x0,
			GVE_SERVICE_TIMER_PERIOD);
		if (err) {
			dev_err(&priv->pdev->dev,
				"Failed to detach stats report: err=%d\n", err);
			gve_trigger_reset(priv);
		}
		err = gve_adminq_deconfigure_device_resources(priv);
		if (err) {
			dev_err(&priv->pdev->dev,
				"Could not deconfigure device resources: err=%d\n",
				err);
			gve_trigger_reset(priv);
		}
	}
	gve_free_counter_array(priv);
	gve_free_notify_blocks(priv);
	gve_free_stats_report(priv);
	gve_clear_device_resources_ok(priv);
}

static void gve_add_napi(struct gve_priv *priv, int ntfy_idx)
{
	struct gve_notify_block *block = &priv->ntfy_blocks[ntfy_idx];

	netif_napi_add(priv->dev, &block->napi, gve_napi_poll,
		       NAPI_POLL_WEIGHT);
}

static void gve_remove_napi(struct gve_priv *priv, int ntfy_idx)
{
	struct gve_notify_block *block = &priv->ntfy_blocks[ntfy_idx];

	netif_napi_del(&block->napi);
}

static int gve_register_qpls(struct gve_priv *priv)
{
	int num_qpls = gve_num_tx_qpls(priv) + gve_num_rx_qpls(priv);
	int err;
	int i;

	for (i = 0; i < num_qpls; i++) {
		err = gve_adminq_register_page_list(priv, &priv->qpls[i]);
		if (err) {
			netif_err(priv, drv, priv->dev,
				  "failed to register queue page list %d\n",
				  priv->qpls[i].id);
			/* This failure will trigger a reset - no need to clean
			 * up
			 */
			return err;
		}
	}
	return 0;
}

static int gve_unregister_qpls(struct gve_priv *priv)
{
	int num_qpls = gve_num_tx_qpls(priv) + gve_num_rx_qpls(priv);
	int err;
	int i;

	for (i = 0; i < num_qpls; i++) {
		err = gve_adminq_unregister_page_list(priv, priv->qpls[i].id);
		/* This failure will trigger a reset - no need to clean up */
		if (err) {
			netif_err(priv, drv, priv->dev,
				  "Failed to unregister queue page list %d\n",
				  priv->qpls[i].id);
			return err;
		}
	}
	return 0;
}

static int gve_create_rings(struct gve_priv *priv)
{
	int err;
	int i;

	err = gve_adminq_create_tx_queues(priv, priv->tx_cfg.num_queues);
	if (err) {
		netif_err(priv, drv, priv->dev, "failed to create %d tx queues\n",
			  priv->tx_cfg.num_queues);
		/* This failure will trigger a reset - no need to clean
		 * up
		 */
		return err;
	}
	netif_dbg(priv, drv, priv->dev, "created %d tx queues \n",
		  priv->tx_cfg.num_queues);

	err = gve_adminq_create_rx_queues(priv, priv->rx_cfg.num_queues);
	if (err) {
		netif_err(priv, drv, priv->dev, "failed to create %d rx queues\n",
			  priv->rx_cfg.num_queues);
		/* This failure will trigger a reset - no need to clean
		 * up
		 */
		return err;
	}
	netif_dbg(priv, drv, priv->dev, "created %d rx queues \n",
		  priv->rx_cfg.num_queues);

	/* Rx data ring has been prefilled with packet buffers at queue
	 * allocation time.
	 * Write the doorbell to provide descriptor slots and packet buffers
	 * to the NIC.
	*/
	for (i = 0; i < priv->rx_cfg.num_queues; i++) {
		gve_rx_write_doorbell(priv, &priv->rx[i]);
	}

	return 0;
}

static int gve_alloc_rings(struct gve_priv *priv)
{
	int ntfy_idx;
	int err;
	int i;

	/* Setup tx rings */
	priv->tx = kvzalloc(priv->tx_cfg.num_queues * sizeof(*priv->tx),
			    GFP_KERNEL);
	if (!priv->tx)
		return -ENOMEM;
	err = gve_tx_alloc_rings(priv);
	if (err)
		goto free_tx;
	/* Setup rx rings */
	priv->rx = kvzalloc(priv->rx_cfg.num_queues * sizeof(*priv->rx),
			    GFP_KERNEL);
	if (!priv->rx) {
		err = -ENOMEM;
		goto free_tx_queue;
	}
	err = gve_rx_alloc_rings(priv);
	if (err)
		goto free_rx;
	/* Add tx napi & init sync stats*/
	for (i = 0; i < priv->tx_cfg.num_queues; i++) {
		u64_stats_init(&priv->tx[i].statss);
		ntfy_idx = gve_tx_idx_to_ntfy(priv, i);
		gve_add_napi(priv, ntfy_idx);
	}
	/* Add rx napi  & init sync stats*/
	for (i = 0; i < priv->rx_cfg.num_queues; i++) {
		u64_stats_init(&priv->rx[i].statss);
		ntfy_idx = gve_rx_idx_to_ntfy(priv, i);
		gve_add_napi(priv, ntfy_idx);
	}

	return 0;

free_rx:
	kfree(priv->rx);
	priv->rx = NULL;
free_tx_queue:
	gve_tx_free_rings(priv);
free_tx:
	kfree(priv->tx);
	priv->tx = NULL;
	return err;
}

static int gve_destroy_rings(struct gve_priv *priv)
{
	int err;

	err = gve_adminq_destroy_tx_queues(priv, priv->tx_cfg.num_queues);
	if (err) {
		netif_err(priv, drv, priv->dev,
			  "failed to destroy tx queues\n");
		/* This failure will trigger a reset - no need to clean up */
		return err;
	}
	netif_dbg(priv, drv, priv->dev, "destroyed tx queues\n");
	err = gve_adminq_destroy_rx_queues(priv, priv->rx_cfg.num_queues);
	if (err) {
		netif_err(priv, drv, priv->dev,
			  "failed to destroy rx queues\n");
		/* This failure will trigger a reset - no need to clean up */
		return err;
	}
	netif_dbg(priv, drv, priv->dev, "destroyed rx queues\n");
	return 0;
}

static void gve_free_rings(struct gve_priv *priv)
{
	int ntfy_idx;
	int i;

	if (priv->tx) {
		for (i = 0; i < priv->tx_cfg.num_queues; i++) {
			ntfy_idx = gve_tx_idx_to_ntfy(priv, i);
			gve_remove_napi(priv, ntfy_idx);
		}
		gve_tx_free_rings(priv);
		kfree(priv->tx);
		priv->tx = NULL;
	}
	if (priv->rx) {
		for (i = 0; i < priv->rx_cfg.num_queues; i++) {
			ntfy_idx = gve_rx_idx_to_ntfy(priv, i);
			gve_remove_napi(priv, ntfy_idx);
		}
		gve_rx_free_rings(priv);
		kfree(priv->rx);
		priv->rx = NULL;
	}
}

int gve_alloc_page(struct gve_priv *priv, struct device *dev,
		   struct page **page, dma_addr_t *dma,
		   enum dma_data_direction dir, gfp_t gfp_flags)
{
	if (priv->dma_mask == 24)
		gfp_flags |= GFP_DMA;
	else if (priv->dma_mask == 32)
		gfp_flags |= GFP_DMA32;

        *page = alloc_page(gfp_flags);
	if (!*page) {
		priv->page_alloc_fail++;
		return -ENOMEM;
	}
	*dma = dma_map_page(dev, *page, 0, PAGE_SIZE, dir);
	if (dma_mapping_error(dev, *dma)) {
		priv->dma_mapping_error++;
		put_page(*page);
		*page = NULL;
		return -ENOMEM;
	}
	return 0;
}

static int gve_alloc_queue_page_list(struct gve_priv *priv, u32 id,
				     int pages)
{
	struct gve_queue_page_list *qpl = &priv->qpls[id];
	int err;
	int i;

	if (pages + priv->num_registered_pages > priv->max_registered_pages) {
		netif_err(priv, drv, priv->dev,
			  "Reached max number of registered pages %llu > %llu\n",
			  pages + priv->num_registered_pages,
			  priv->max_registered_pages);
		return -EINVAL;
	}

	qpl->id = id;
	qpl->num_entries = 0;
	qpl->pages = kvzalloc(pages * sizeof(*qpl->pages), GFP_KERNEL);
	/* caller handles clean up */
	if (!qpl->pages)
		return -ENOMEM;
	qpl->page_buses = kvzalloc(pages * sizeof(*qpl->page_buses),
				   GFP_KERNEL);
	/* caller handles clean up */
	if (!qpl->page_buses)
		return -ENOMEM;

	for (i = 0; i < pages; i++) {
		err = gve_alloc_page(priv, &priv->pdev->dev, &qpl->pages[i],
				     &qpl->page_buses[i],
				     gve_qpl_dma_dir(priv, id), GFP_KERNEL);
		/* caller handles clean up */
		if (err)
			return -ENOMEM;
		qpl->num_entries++;
	}
	priv->num_registered_pages += pages;

	return 0;
}

void gve_free_page(struct device *dev, struct page *page, dma_addr_t dma,
		   enum dma_data_direction dir)
{
	if (!dma_mapping_error(dev, dma))
		dma_unmap_page(dev, dma, PAGE_SIZE, dir);
	if (page)
		put_page(page);
}

static void gve_free_queue_page_list(struct gve_priv *priv, u32 id)
{
	struct gve_queue_page_list *qpl = &priv->qpls[id];
	int i;

	if (!qpl->pages)
		return;
	if (!qpl->page_buses)
		goto free_pages;

	for (i = 0; i < qpl->num_entries; i++)
		gve_free_page(&priv->pdev->dev, qpl->pages[i],
			      qpl->page_buses[i], gve_qpl_dma_dir(priv, id));

	kfree(qpl->page_buses);
free_pages:
	kfree(qpl->pages);
	priv->num_registered_pages -= qpl->num_entries;

}

static int gve_alloc_qpls(struct gve_priv *priv)
{
	int num_qpls = gve_num_tx_qpls(priv) + gve_num_rx_qpls(priv);
	int i, j;
	int err;

	/* Raw addressing means no QPLs */
	if (priv->raw_addressing)
		return 0;

	priv->qpls = kvzalloc(num_qpls * sizeof(*priv->qpls), GFP_KERNEL);
	if (!priv->qpls)
		return -ENOMEM;

	for (i = 0; i < gve_num_tx_qpls(priv); i++) {
		err = gve_alloc_queue_page_list(priv, i,
						priv->tx_pages_per_qpl);
		if (err)
			goto free_qpls;
	}
	for (; i < num_qpls; i++) {
		err = gve_alloc_queue_page_list(priv, i,
						priv->rx_data_slot_cnt);
		if (err)
			goto free_qpls;
	}

	priv->qpl_cfg.qpl_map_size = BITS_TO_LONGS(num_qpls) *
				     sizeof(unsigned long) * BITS_PER_BYTE;
	priv->qpl_cfg.qpl_id_map = kvzalloc(BITS_TO_LONGS(num_qpls) *
					    sizeof(unsigned long), GFP_KERNEL);
	if (!priv->qpl_cfg.qpl_id_map)
		goto free_qpls;

	return 0;

free_qpls:
	for (j = 0; j <= i; j++)
		gve_free_queue_page_list(priv, j);
	kfree(priv->qpls);
	return err;
}

static void gve_free_qpls(struct gve_priv *priv)
{
	int num_qpls = gve_num_tx_qpls(priv) + gve_num_rx_qpls(priv);
	int i;

	/* Raw addressing means no QPLs */
	if (priv->raw_addressing)
		return;

	kfree(priv->qpl_cfg.qpl_id_map);

	for (i = 0; i < num_qpls; i++)
		gve_free_queue_page_list(priv, i);

	kfree(priv->qpls);
}

/* Use this to schedule a reset when the device is capable of continuing
 * to handle other requests in its current state. If it is not, do a reset
 * in thread instead.
 */
void gve_schedule_reset(struct gve_priv *priv)
{
	gve_set_do_reset(priv);
	queue_work(priv->gve_wq, &priv->service_task);
}

static void gve_reset_and_teardown(struct gve_priv *priv, bool was_up);
static int gve_reset_recovery(struct gve_priv *priv, bool was_up);
static void gve_turndown(struct gve_priv *priv);
static void gve_turnup(struct gve_priv *priv);

static int gve_open(struct net_device *dev)
{
	struct gve_priv *priv = netdev_priv(dev);
	int err;

	err = gve_alloc_qpls(priv);
	if (err)
		return err;
	err = gve_alloc_rings(priv);
	if (err)
		goto free_qpls;

	err = netif_set_real_num_tx_queues(dev, priv->tx_cfg.num_queues);
	if (err)
		goto free_rings;
	err = netif_set_real_num_rx_queues(dev, priv->rx_cfg.num_queues);
	if (err)
		goto free_rings;

	err = gve_register_qpls(priv);
	if (err)
		goto reset;
	err = gve_create_rings(priv);
	if (err)
		goto reset;
	gve_set_device_rings_ok(priv);

	gve_turnup(priv);
	queue_work(priv->gve_wq, &priv->service_task);
	priv->interface_up_cnt++;
	return 0;

free_rings:
	gve_free_rings(priv);
free_qpls:
	gve_free_qpls(priv);
	return err;

reset:
	/* This must have been called from a reset due to the rtnl lock
	 * so just return at this point.
	 */
	if (gve_get_reset_in_progress(priv))
		return err;
	/* Otherwise reset before returning */
	gve_reset_and_teardown(priv, true);
	/* if this fails there is nothing we can do so just ignore the return */
	gve_reset_recovery(priv, false);
	/* return the original error */
	return err;
}

static int gve_close(struct net_device *dev)
{
	struct gve_priv *priv = netdev_priv(dev);
	int err;

	netif_carrier_off(dev);
	if (gve_get_device_rings_ok(priv)) {
		gve_turndown(priv);
		err = gve_destroy_rings(priv);
		if (err)
			goto err;
		err = gve_unregister_qpls(priv);
		if (err)
			goto err;
		gve_clear_device_rings_ok(priv);
	}

	gve_free_rings(priv);
	gve_free_qpls(priv);
	priv->interface_down_cnt++;
	return 0;

err:
	/* This must have been called from a reset due to the rtnl lock
	 * so just return at this point.
	 */
	if (gve_get_reset_in_progress(priv))
		return err;
	/* Otherwise reset before returning */
	gve_reset_and_teardown(priv, true);
	return gve_reset_recovery(priv, false);
}

int gve_adjust_queues(struct gve_priv *priv,
		      struct gve_queue_config new_rx_config,
		      struct gve_queue_config new_tx_config)
{
	int err;

	if (netif_carrier_ok(priv->dev)) {
		/* To make this process as simple as possible we teardown the
		 * device, set the new configuration, and then bring the device
		 * up again.
		 */
		err = gve_close(priv->dev);
		/* we have already tried to reset in close,
		 * just fail at this point
		 */
		if (err)
			return err;
		priv->tx_cfg = new_tx_config;
		priv->rx_cfg = new_rx_config;

		err = gve_open(priv->dev);
		if (err)
			goto err;

		return 0;
	}
	/* Set the config for the next up. */
	priv->tx_cfg = new_tx_config;
	priv->rx_cfg = new_rx_config;

	return 0;
err:
	netif_err(priv, drv, priv->dev,
		  "Adjust queues failed! !!! DISABLING ALL QUEUES !!!\n");
	gve_turndown(priv);
	return err;
}

static void gve_turndown(struct gve_priv *priv)
{
	int idx;

	if (netif_carrier_ok(priv->dev))
		netif_carrier_off(priv->dev);

	if (!gve_get_napi_enabled(priv))
		return;

	/* Disable napi to prevent more work from coming in */
	for (idx = 0; idx < priv->tx_cfg.num_queues; idx++) {
		int ntfy_idx = gve_tx_idx_to_ntfy(priv, idx);
		struct gve_notify_block *block = &priv->ntfy_blocks[ntfy_idx];

		napi_disable(&block->napi);
	}
	for (idx = 0; idx < priv->rx_cfg.num_queues; idx++) {
		int ntfy_idx = gve_rx_idx_to_ntfy(priv, idx);
		struct gve_notify_block *block = &priv->ntfy_blocks[ntfy_idx];

		napi_disable(&block->napi);
	}

	/* Stop tx queues */
	netif_tx_disable(priv->dev);

	gve_clear_napi_enabled(priv);
	gve_clear_report_stats(priv);
}

static void gve_turnup(struct gve_priv *priv)
{
	int idx;

	/* Start the tx queues */
	netif_tx_start_all_queues(priv->dev);

	/* Enable napi and unmask interrupts for all queues */
	for (idx = 0; idx < priv->tx_cfg.num_queues; idx++) {
		int ntfy_idx = gve_tx_idx_to_ntfy(priv, idx);
		struct gve_notify_block *block = &priv->ntfy_blocks[ntfy_idx];

		napi_enable(&block->napi);
		iowrite32be(0, gve_irq_doorbell(priv, block));
	}
	for (idx = 0; idx < priv->rx_cfg.num_queues; idx++) {
		int ntfy_idx = gve_rx_idx_to_ntfy(priv, idx);
		struct gve_notify_block *block = &priv->ntfy_blocks[ntfy_idx];

		napi_enable(&block->napi);
		iowrite32be(0, gve_irq_doorbell(priv, block));
	}

	gve_set_napi_enabled(priv);
}

static void gve_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct gve_priv *priv = netdev_priv(dev);
	int i, j;

	rtnl_lock();
	// LOG Driver state
	dev_info(&priv->pdev->dev, "gve::adminq_mask=%u", priv->adminq_mask);
	dev_info(&priv->pdev->dev, "gve::adminq_prod_cnt=%u", priv->adminq_prod_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_cmd_fail=%u", priv->adminq_cmd_fail);
	dev_info(&priv->pdev->dev, "gve::adminq_timeouts=%u", priv->adminq_timeouts);
	dev_info(&priv->pdev->dev, "gve::adminq_describe_device_cnt=%u", priv->adminq_describe_device_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_cfg_device_resources_cnt=%u", priv->adminq_cfg_device_resources_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_register_page_list_cnt=%u", priv->adminq_register_page_list_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_unregister_page_list_cnt=%u", priv->adminq_unregister_page_list_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_create_tx_queue_cnt=%u", priv->adminq_create_tx_queue_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_create_rx_queue_cnt=%u", priv->adminq_create_rx_queue_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_destroy_tx_queue_cnt=%u", priv->adminq_destroy_tx_queue_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_destroy_rx_queue_cnt=%u", priv->adminq_destroy_rx_queue_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_dcfg_device_resources_cnt=%u", priv->adminq_dcfg_device_resources_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_set_driver_parameter_cnt=%u", priv->adminq_set_driver_parameter_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_report_stats_cnt=%u", priv->adminq_report_stats_cnt);
	dev_info(&priv->pdev->dev, "gve::adminq_report_link_speed_cnt=%u", priv->adminq_report_link_speed_cnt);
	dev_info(&priv->pdev->dev, "gve::interface_up_cnt=%u", priv->interface_up_cnt);
	dev_info(&priv->pdev->dev, "gve::interface_down_cnt=%u", priv->interface_down_cnt);
	dev_info(&priv->pdev->dev, "gve::reset_cnt=%u", priv->reset_cnt);
	dev_info(&priv->pdev->dev, "gve::page_alloc_fail=%u", priv->page_alloc_fail);
	dev_info(&priv->pdev->dev, "gve::dma_mapping_error=%u", priv->dma_mapping_error);
	dev_info(&priv->pdev->dev, "gve::suspend_cnt=%u", priv->suspend_cnt);
	dev_info(&priv->pdev->dev, "gve::resume_cnt=%u", priv->resume_cnt);
	dev_info(&priv->pdev->dev, "gve::service_task_flags=%lu", priv->service_task_flags);
	dev_info(&priv->pdev->dev, "gve::state_flags=%lu", priv->state_flags);
	dev_info(&priv->pdev->dev, "gve::ethtool_flags=%lu", priv->ethtool_flags);

	for (i = 0; i < priv->tx_cfg.num_queues; i++) {
		struct gve_tx_ring *tx = &priv->tx[i];

		dev_info(&priv->pdev->dev, "gve::tx[%i].tx_fifo.size=%u", i, tx->tx_fifo.size);
		dev_info(&priv->pdev->dev, "gve::tx[%i].tx_fifo.available=%u", i, atomic_read(&tx->tx_fifo.available));
		dev_info(&priv->pdev->dev, "gve::tx[%i].tx_fifo.head=%u", i, tx->tx_fifo.head);
		dev_info(&priv->pdev->dev, "gve::tx[%i].req=%u", i, tx->req);
		dev_info(&priv->pdev->dev, "gve::tx[%i].done=%u", i, tx->done);
		dev_info(&priv->pdev->dev, "gve::tx[%i].last_nic_done=%u", i, be32_to_cpu(tx->last_nic_done));
		dev_info(&priv->pdev->dev, "gve::tx[%i].last_nic_done_irq=%u", i, be32_to_cpu(tx->last_nic_done_irq));
		dev_info(&priv->pdev->dev, "gve::tx[%i].last_nic_done_clear=%u", i, be32_to_cpu(tx->last_nic_done_clear));
		dev_info(&priv->pdev->dev, "gve::tx[%i].last_nic_done_tx_poll=%u", i, be32_to_cpu(tx->last_nic_done_tx_poll));
		dev_info(&priv->pdev->dev, "gve::tx[%i].pkt_done=%llu", i, tx->pkt_done);
		dev_info(&priv->pdev->dev, "gve::tx[%i].dropped_pkt=%u", i, tx->dropped_pkt);

		dev_info(&priv->pdev->dev, "gve::tx[%i].queue_resources(db_index, db_counter)=(%u, %u)", i, be32_to_cpu(tx->q_resources->db_index), be32_to_cpu(tx->q_resources->counter_index));
		dev_info(&priv->pdev->dev, "gve::tx[%i]: db_bar2[db_index]=%u", i, ioread32be(&priv->db_bar2[be32_to_cpu(tx->q_resources->db_index)]));
		dev_info(&priv->pdev->dev, "gve::tx[%i]: counter_array[counter_index]=%u", i, be32_to_cpu(gve_tx_load_event_counter(priv, tx)));
		dev_info(&priv->pdev->dev, "gve::tx[%i]: db_bar2[irq_index]=%u", i, ioread32be(gve_irq_doorbell(priv, &priv->ntfy_blocks[gve_tx_idx_to_ntfy(priv, tx->q_num)])));
		dev_info(&priv->pdev->dev, "gve::tx[%i].stop_queue=%u", i, tx->stop_queue);
		dev_info(&priv->pdev->dev, "gve::tx[%i].wake_queue=%u", i, tx->wake_queue);
		dev_info(&priv->pdev->dev, "gve::tx[%i].ntfy_id=%u", i, tx->ntfy_id);
		dev_info(&priv->pdev->dev, "gve::tx[%i].netdev_txq->state=%u", i, tx->netdev_txq->state);
	}

	for (i = 0; i < priv->rx_cfg.num_queues; i++) {
		struct gve_rx_ring *rx = &priv->rx[i];

		dev_info(&priv->pdev->dev, "gve::rx[%i].desc.seqno=%u", i, rx->desc.seqno);
		dev_info(&priv->pdev->dev, "gve::rx[%i].data.rda=%u", i, rx->data.raw_addressing);
		dev_info(&priv->pdev->dev, "gve::rx[%i].data.page_info.page_offset=%u", i, rx->data.page_info->page_offset);
		dev_info(&priv->pdev->dev, "gve::rx[%i].data.page_info.pagecnt_bias=%u", i, rx->data.page_info->pagecnt_bias);
		dev_info(&priv->pdev->dev, "gve::rx[%i].data.page_info.can_flip=%u", i, rx->data.page_info->can_flip);


		dev_info(&priv->pdev->dev, "gve::rx[%i].rbytes=%llu", i, rx->rbytes);
		dev_info(&priv->pdev->dev, "gve::rx[%i].rpackets=%llu", i, rx->rpackets);
		dev_info(&priv->pdev->dev, "gve::rx[%i].cnt=%u", i, rx->cnt);
		dev_info(&priv->pdev->dev, "gve::rx[%i].fill_cnt=%u", i, rx->fill_cnt);
		dev_info(&priv->pdev->dev, "gve::rx[%i].mask=%u", i, rx->mask);
		dev_info(&priv->pdev->dev, "gve::rx[%i].db_threshold=%u", i, rx->db_threshold);
		dev_info(&priv->pdev->dev, "gve::rx[%i].rx_copybreak_pkt=%llu", i, rx->rx_copybreak_pkt);
		dev_info(&priv->pdev->dev, "gve::rx[%i].rx_copied_pkt=%llu", i, rx->rx_copied_pkt);
		dev_info(&priv->pdev->dev, "gve::rx[%i].rx_skb_alloc_fail=%llu", i, rx->rx_skb_alloc_fail);
		dev_info(&priv->pdev->dev, "gve::rx[%i].rx_buf_alloc_fail=%llu", i, rx->rx_buf_alloc_fail);
		dev_info(&priv->pdev->dev, "gve::rx[%i].rx_desc_err_dropped_pkt=%llu", i, rx->rx_desc_err_dropped_pkt);
		dev_info(&priv->pdev->dev, "gve::rx[%i].rx_no_refill_dropped_pkt=%llu", i, rx->rx_no_refill_dropped_pkt);
		dev_info(&priv->pdev->dev, "gve::rx[%i].q_num=%u", i, rx->q_num);
		dev_info(&priv->pdev->dev, "gve::rx[%i].ntfy_id=%u", i, rx->ntfy_id);
		dev_info(&priv->pdev->dev, "gve::rx[%i].queue_resources(db_index, db_counter)=(%u, %u)", i, be32_to_cpu(rx->q_resources->db_index), be32_to_cpu(rx->q_resources->counter_index));
		dev_info(&priv->pdev->dev, "gve::rx[%i]: db_bar2[db_index]=%u", i, ioread32be(&priv->db_bar2[be32_to_cpu(rx->q_resources->db_index)]));
	}

#define GVE_DUMP_TX_DESCS
#define GVE_DUMP_RX_DESCS

#ifdef GVE_DUMP_TX_DESCS
		for (i = 0; i < priv->tx_cfg.num_queues; i++) {
			struct gve_tx_ring *tx = &priv->tx[i];

			for (j = 0; j < priv->tx_desc_cnt; j++) {
				union gve_tx_desc *desc = &tx->desc[j];
				if (desc->pkt.type_flags & GVE_TXD_TSO) {
					dev_info(&priv->pdev->dev, "gve::tx[%i].desc[%i]: type_flags=%x,l4_csum_offset=%i,l4_hdr_offset=%i,desc_cnt=%i,len=%i,seg_len=%i,seg_addr=%i", i, j, desc->pkt.type_flags, desc->pkt.l4_csum_offset, desc->pkt.l4_hdr_offset, desc->pkt.desc_cnt, be16_to_cpu(desc->pkt.len), be16_to_cpu(desc->pkt.seg_len), be16_to_cpu(desc->pkt.seg_addr));
				} else {
					dev_info(&priv->pdev->dev, "gve::tx[%i].desc[%i]: type_flags=%x,l3_offset=%i,mss=%i,seg_len=%i,seg_addr=%i", i, j, desc->pkt.type_flags, desc->seg.l3_offset, be16_to_cpu(desc->seg.mss), be16_to_cpu(desc->seg.seg_len), be16_to_cpu(desc->seg.seg_addr));
				}
			}
		}
#endif
#ifdef GVE_DUMP_RX_DESCS
		for (i = 0; i < priv->rx_cfg.num_queues; i++) {
		struct gve_rx_ring *rx = &priv->rx[i];
			for (j = 0; j < priv->rx_desc_cnt; j++) {
				struct gve_rx_desc *desc = &rx->desc.desc_ring[j];

				dev_info(&priv->pdev->dev, "gve::rx[%i].desc[%i]: hdr_off=%u, hdr_len=%u, len=%u, flags_seq=%u", i, j, desc->hdr_len, desc->hdr_len, be16_to_cpu(desc->len), be16_to_cpu(desc->flags_seq));
			}
		}
#endif
	rtnl_unlock();
	gve_schedule_reset(priv);
	priv->tx_timeo_cnt++;
}

static const struct net_device_ops gve_netdev_ops = {
	.ndo_start_xmit		=	gve_tx,
	.ndo_open		=	gve_open,
	.ndo_stop		=	gve_close,
	.ndo_get_stats64	=	gve_get_stats,
	.ndo_tx_timeout         =       gve_tx_timeout,
};

static void gve_handle_status(struct gve_priv *priv, u32 status)
{
	if (GVE_DEVICE_STATUS_RESET_MASK & status) {
		dev_info(&priv->pdev->dev, "Device requested reset.\n");
		gve_set_do_reset(priv);
	}
	if (GVE_DEVICE_STATUS_REPORT_STATS_MASK & status) {
		dev_info(&priv->pdev->dev, "Device report stats on.\n");
		gve_set_do_report_stats(priv);
	}
}

static void gve_handle_reset(struct gve_priv *priv)
{
	/* A service task will be scheduled at the end of probe to catch any
	 * resets that need to happen, and we don't want to reset until
	 * probe is done.
	 */
	if (gve_get_probe_in_progress(priv))
		return;

	if (gve_get_do_reset(priv)) {
		rtnl_lock();
		gve_reset(priv, false);
		rtnl_unlock();
	}
}

void gve_handle_report_stats(struct gve_priv *priv)
{
	int idx, stats_idx = 0, tx_bytes;
	unsigned int start = 0;
	struct stats *stats = priv->stats_report->stats;

	if (!gve_get_report_stats(priv))
		return;

	be64_add_cpu(&priv->stats_report->written_count, 1);
	/* tx stats */
	if (priv->tx) {
		for (idx = 0; idx < priv->tx_cfg.num_queues; idx++) {
			do {
				start = u64_stats_fetch_begin(&priv->tx[idx].statss);
				tx_bytes = priv->tx[idx].bytes_done;
			} while (u64_stats_fetch_retry(&priv->tx[idx].statss, start));
			stats[stats_idx++] = (struct stats) {
				.stat_name = cpu_to_be32(TX_WAKE_CNT),
				.value = cpu_to_be64(priv->tx[idx].wake_queue),
				.queue_id = cpu_to_be32(idx),
			};
			stats[stats_idx++] = (struct stats) {
				.stat_name = cpu_to_be32(TX_STOP_CNT),
				.value = cpu_to_be64(priv->tx[idx].stop_queue),
				.queue_id = cpu_to_be32(idx),
			};
			stats[stats_idx++] = (struct stats) {
				.stat_name = cpu_to_be32(TX_FRAMES_SENT),
				.value = cpu_to_be64(priv->tx[idx].req),
				.queue_id = cpu_to_be32(idx),
			};
			stats[stats_idx++] = (struct stats) {
				.stat_name = cpu_to_be32(TX_BYTES_SENT),
				.value = cpu_to_be64(tx_bytes),
				.queue_id = cpu_to_be32(idx),
			};
			stats[stats_idx++] = (struct stats) {
				.stat_name = cpu_to_be32(
					TX_LAST_COMPLETION_PROCESSED),
				.value = cpu_to_be64(priv->tx[idx].done),
				.queue_id = cpu_to_be32(idx),
			};
		}
	}
	/* rx stats */
	if (priv->rx) {
		for (idx = 0; idx < priv->rx_cfg.num_queues; idx++) {
			stats[stats_idx++] = (struct stats) {
				.stat_name = cpu_to_be32(
					RX_NEXT_EXPECTED_SEQUENCE),
				.value = cpu_to_be64(priv->rx[idx].desc.seqno),
				.queue_id = cpu_to_be32(idx),
			};
			stats[stats_idx++] = (struct stats) {
				.stat_name = cpu_to_be32(RX_BUFFERS_POSTED),
				.value = cpu_to_be64(priv->rx[0].fill_cnt),
				.queue_id = cpu_to_be32(idx),
			};
		}
	}
}

void gve_handle_link_status(struct gve_priv *priv, bool link_status)
{
	if (!gve_get_napi_enabled(priv))
		return;

	if (link_status == netif_carrier_ok(priv->dev))
		return;

	if (link_status) {
		netif_carrier_on(priv->dev);
	} else {
		dev_info(&priv->pdev->dev, "Device link is down.\n");
		netif_carrier_off(priv->dev);
	}
}

/* Handle NIC status register changes, reset requests and report stats */
static void gve_service_task(struct work_struct *work)
{
	struct gve_priv *priv = container_of(work, struct gve_priv,
					     service_task);
	u32 status = ioread32be(&priv->reg_bar0->device_status);

	gve_handle_status(priv, status);

	gve_handle_reset(priv);
	gve_handle_link_status(priv, GVE_DEVICE_STATUS_LINK_STATUS_MASK & status);
	if (gve_get_do_report_stats(priv)) {
		gve_handle_report_stats(priv);
		gve_clear_do_report_stats(priv);
	}
}

static int gve_init_priv(struct gve_priv *priv, bool skip_describe_device)
{
	int num_ntfy;
	int err;

	/* Set up the adminq */
	err = gve_adminq_alloc(&priv->pdev->dev, priv);
	if (err) {
		dev_err(&priv->pdev->dev,
			"Failed to alloc admin queue: err=%d\n", err);
		return err;
	}

	if (skip_describe_device)
		goto setup_device;

	priv->raw_addressing = false;
	/* Get the initial information we need from the device */
	err = gve_adminq_describe_device(priv);
	if (err) {
		dev_err(&priv->pdev->dev,
			"Could not get device information: err=%d\n", err);
		goto err;
	}
	if (priv->dev->max_mtu > PAGE_SIZE) {
		priv->dev->max_mtu = PAGE_SIZE;
		err = gve_adminq_set_mtu(priv, priv->dev->mtu);
		if (err) {
		        dev_err(&priv->pdev->dev, "Could not set mtu");
			goto err;
		}
	}
	priv->dev->mtu = priv->dev->max_mtu;
	num_ntfy = pci_msix_vec_count(priv->pdev);
	if (num_ntfy <= 0) {
		dev_err(&priv->pdev->dev,
			"could not count MSI-x vectors: err=%d\n", num_ntfy);
		err = num_ntfy;
		goto err;
	} else if (num_ntfy < GVE_MIN_MSIX) {
		dev_err(&priv->pdev->dev, "gve needs at least %d MSI-x vectors, but only has %d\n",
			GVE_MIN_MSIX, num_ntfy);
		err = -EINVAL;
		goto err;
	}

	priv->num_registered_pages = 0;
	priv->rx_copybreak = GVE_DEFAULT_RX_COPYBREAK;
	/* gvnic has one Notification Block per MSI-x vector, except for the
	 * management vector
	 */
	priv->num_ntfy_blks = (num_ntfy - 1) & ~0x1;
	priv->mgmt_msix_idx = priv->num_ntfy_blks;

	priv->tx_cfg.max_queues =
		min_t(int, priv->tx_cfg.max_queues, priv->num_ntfy_blks / 2);
	priv->rx_cfg.max_queues =
		min_t(int, priv->rx_cfg.max_queues, priv->num_ntfy_blks / 2);

	priv->tx_cfg.num_queues = priv->tx_cfg.max_queues;
	priv->rx_cfg.num_queues = priv->rx_cfg.max_queues;
	if (priv->default_num_queues > 0) {
		priv->tx_cfg.num_queues = min_t(int, priv->default_num_queues,
						priv->tx_cfg.num_queues);
		priv->rx_cfg.num_queues = min_t(int, priv->default_num_queues,
						priv->rx_cfg.num_queues);
	}

	dev_info(&priv->pdev->dev, "TX queues %d, RX queues %d\n",
		 priv->tx_cfg.num_queues, priv->rx_cfg.num_queues);
	dev_info(&priv->pdev->dev, "Max TX queues %d, Max RX queues %d\n",
		 priv->tx_cfg.max_queues, priv->rx_cfg.max_queues);

setup_device:
	err = gve_setup_device_resources(priv);
	if (!err)
		return 0;
err:
	gve_adminq_free(&priv->pdev->dev, priv);
	return err;
}

static void gve_teardown_priv_resources(struct gve_priv *priv)
{
	gve_teardown_device_resources(priv);
	gve_adminq_free(&priv->pdev->dev, priv);
}

static void gve_trigger_reset(struct gve_priv *priv)
{
	/* Reset the device by releasing the AQ */
	gve_adminq_release(priv);
}

static void gve_reset_and_teardown(struct gve_priv *priv, bool was_up)
{
	gve_trigger_reset(priv);
	/* With the reset having already happened, close cannot fail */
	if (was_up)
		gve_close(priv->dev);
	gve_teardown_priv_resources(priv);
}

static int gve_reset_recovery(struct gve_priv *priv, bool was_up)
{
	int err;

	err = gve_init_priv(priv, true);
	if (err)
		goto err;
	if (was_up) {
		err = gve_open(priv->dev);
		if (err)
			goto err;
	}
	return 0;
err:
	dev_err(&priv->pdev->dev, "Reset failed! !!! DISABLING ALL QUEUES !!!\n");
	gve_turndown(priv);
	return err;
}

int gve_reset(struct gve_priv *priv, bool attempt_teardown)
{
	bool was_up = netif_carrier_ok(priv->dev);
	int err;

	dev_info(&priv->pdev->dev, "Performing reset\n");
	gve_clear_do_reset(priv);
	gve_set_reset_in_progress(priv);
	/* If we aren't attempting to teardown normally, just go turndown and
	 * reset right away.
	 */
	if (!attempt_teardown) {
		gve_turndown(priv);
		gve_reset_and_teardown(priv, was_up);
	} else {
		/* Otherwise attempt to close normally */
		if (was_up) {
			err = gve_close(priv->dev);
			/* If that fails reset as we did above */
			if (err)
				gve_reset_and_teardown(priv, was_up);
		}
		/* Clean up any remaining resources */
		gve_teardown_priv_resources(priv);
	}

	/* Set it all back up */
	err = gve_reset_recovery(priv, was_up);
	gve_clear_reset_in_progress(priv);
	priv->reset_cnt++;
	priv->interface_up_cnt = 0;
	priv->interface_down_cnt = 0;
	return err;
}

static void gve_write_version(u8 __iomem *driver_version_register)
{
	const char *c = gve_version_prefix;

	while (*c) {
		writeb(*c, driver_version_register);
		c++;
	}

	c = gve_version_str;
	while (*c) {
		writeb(*c, driver_version_register);
		c++;
	}
	writeb('\n', driver_version_register);
}

static int gve_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	int max_tx_queues, max_rx_queues;
	struct net_device *dev;
	__be32 __iomem *db_bar;
	struct gve_registers __iomem *reg_bar;
	struct gve_priv *priv;
	u8 dma_mask;
	int err;

	err = pci_enable_device(pdev);
	if (err)
		return -ENXIO;

	err = pci_request_regions(pdev, "gvnic-cfg");
	if (err)
		goto abort_with_enabled;

	pci_set_master(pdev);

	reg_bar = pci_iomap(pdev, GVE_REGISTER_BAR, 0);
	if (!reg_bar) {
		dev_err(&pdev->dev, "Failed to map pci bar!\n");
		err = -ENOMEM;
		goto abort_with_pci_region;
	}

	db_bar = pci_iomap(pdev, GVE_DOORBELL_BAR, 0);
	if (!db_bar) {
		dev_err(&pdev->dev, "Failed to map doorbell bar!\n");
		err = -ENOMEM;
		goto abort_with_reg_bar;
	}

	dma_mask = readb(&reg_bar->dma_mask);
	// Default to 64 if the register isn't set
	if (!dma_mask)
		dma_mask = 64;
	gve_write_version(&reg_bar->driver_version);
	/* Get max queues to alloc etherdev */
	max_tx_queues = ioread32be(&reg_bar->max_tx_queues);
	max_rx_queues = ioread32be(&reg_bar->max_rx_queues);

	err = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(&pdev->dev, "Failed to set dma mask: err=%d\n", err);
		goto abort_with_reg_bar;
	}

	err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(&pdev->dev,
			"Failed to set consistent dma mask: err=%d\n", err);
		goto abort_with_reg_bar;
	}

	/* Alloc and setup the netdev and priv */
	dev = alloc_etherdev_mqs(sizeof(*priv), max_tx_queues, max_rx_queues);
	if (!dev) {
		dev_err(&pdev->dev, "could not allocate netdev\n");
		goto abort_with_db_bar;
	}
	SET_NETDEV_DEV(dev, &pdev->dev);

	pci_set_drvdata(pdev, dev);

	dev->ethtool_ops = &gve_ethtool_ops;
	dev->netdev_ops = &gve_netdev_ops;
	/* advertise features */
	dev->hw_features = NETIF_F_HIGHDMA;
	dev->hw_features |= NETIF_F_SG;
	dev->hw_features |= NETIF_F_HW_CSUM;
	dev->hw_features |= NETIF_F_TSO;
	dev->hw_features |= NETIF_F_TSO6;
	dev->hw_features |= NETIF_F_TSO_ECN;
	dev->hw_features |= NETIF_F_RXCSUM;
	dev->hw_features |= NETIF_F_RXHASH;
	dev->features = dev->hw_features;
	dev->watchdog_timeo = 5 * HZ;
	dev->min_mtu = ETH_MIN_MTU;
	netif_carrier_off(dev);

	priv = netdev_priv(dev);
	priv->dev = dev;
	priv->pdev = pdev;
	priv->msg_enable = DEFAULT_MSG_LEVEL;
	priv->reg_bar0 = reg_bar;
	priv->db_bar2 = db_bar;
	priv->service_task_flags = 0x0;
	priv->state_flags = 0x0;
	priv->ethtool_flags = 0x0;
        priv->dma_mask = dma_mask;

	gve_set_probe_in_progress(priv);

	priv->gve_wq = alloc_ordered_workqueue("gve", 0);
	if (!priv->gve_wq) {
		dev_err(&pdev->dev, "Could not allocate workqueue");
		err = -ENOMEM;
		goto abort_with_netdev;
	}
	INIT_WORK(&priv->service_task, gve_service_task);
	priv->tx_cfg.max_queues = max_tx_queues;
	priv->rx_cfg.max_queues = max_rx_queues;

	err = gve_init_priv(priv, false);
	if (err)
		goto abort_with_wq;

	err = register_netdev(dev);
  if (err)
		goto abort_with_wq;

	dev_info(&pdev->dev, "GVE version %s\n", gve_version_str);
	gve_clear_probe_in_progress(priv);
	queue_work(priv->gve_wq, &priv->service_task);

	return 0;

abort_with_wq:
	destroy_workqueue(priv->gve_wq);

abort_with_netdev:
	free_netdev(dev);

abort_with_db_bar:
	pci_iounmap(pdev, db_bar);

abort_with_reg_bar:
	pci_iounmap(pdev, reg_bar);

abort_with_pci_region:
	pci_release_regions(pdev);

abort_with_enabled:
	pci_disable_device(pdev);
	return -ENXIO;
}
EXPORT_SYMBOL(gve_probe);

static void gve_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct gve_priv *priv = netdev_priv(netdev);
	__be32 __iomem *db_bar = priv->db_bar2;
	void __iomem *reg_bar = priv->reg_bar0;

	unregister_netdev(netdev);
	gve_teardown_priv_resources(priv);
	destroy_workqueue(priv->gve_wq);
	free_netdev(netdev);
	pci_iounmap(pdev, db_bar);
	pci_iounmap(pdev, reg_bar);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

#ifdef CONFIG_PM
static int gve_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct gve_priv *priv = netdev_priv(netdev);
	bool was_up = netif_carrier_ok(priv->dev);

	priv->suspend_cnt++;
	rtnl_lock();
	if (was_up && gve_close(priv->dev)) {
		/* If the dev was up, attempt to close, if close fails, reset */
		gve_reset_and_teardown(priv, was_up);
	} else {
		/* If the dev wasn't up or close worked, finish tearing down */
		gve_teardown_priv_resources(priv);
	}
	priv->up_before_suspend = was_up;
	rtnl_unlock();
	return 0;
}

static int gve_resume(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct gve_priv *priv = netdev_priv(netdev);
	int err;

	priv->resume_cnt++;
	rtnl_lock();
	err = gve_reset_recovery(priv, priv->up_before_suspend);
	rtnl_unlock();
	return err;
}
#endif /* CONFIG_PM */

static const struct pci_device_id gve_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_GOOGLE, PCI_DEV_ID_GVNIC) },
	{ }
};

static struct pci_driver gvnic_driver = {
	.name		= "gvnic",
	.id_table	= gve_id_table,
	.probe		= gve_probe,
	.remove		= gve_remove,
#ifdef CONFIG_PM
	.suspend	= gve_suspend,
	.resume		= gve_resume,
#endif
};

module_pci_driver(gvnic_driver);

MODULE_DEVICE_TABLE(pci, gve_id_table);
MODULE_AUTHOR("Google, Inc.");
MODULE_DESCRIPTION("gVNIC Driver");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION(GVE_VERSION);
