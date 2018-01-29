/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   BSD LICENSE
 *
 *   Copyright(c)
 *            2015-2016 George Washington University
 *            2015-2016 University of California Riverside
 *            2010-2014 Intel Corporation. All rights reserved.
 *            2016 Hewlett Packard Enterprise Development LP
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * The name of the author may not be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
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
 *
 ********************************************************************/


/******************************************************************************
                                   main.c

     File containing the main function of the manager and all its worker
     threads.

******************************************************************************/


#include <signal.h>

#include "onvm_mgr.h"
#include "onvm_stats.h"
#include "onvm_pkt.h"
#include "onvm_nf.h"
#include "fastpath_pkt.h"
#include "sa_snort.h"

/****************************FP Global Variables****************************/

int LMAT_bef_cons[(NUM_OF_NF)+1][6];
uint32_t hash_fid;
int cpa[4];
int *tmp_cpa;
uint64_t cyc_start, cyc_end;
uint64_t cyc_start_1, cyc_end_1;
uint64_t cyc_start_2, cyc_end_2;
uint64_t cyc_start_3, cyc_end_3;
uint64_t hz;
/****************************FP Snort Variables****************************/
int file_line;      /* current line being processed in the rules file */
int rule_count;
Rule *current;      /* util ptr for the current rule */
Rule *PassList;     /* List of Pass Rules */
Rule *LogList;      /* List of Log Rules */
Rule *AlertList;    /* List of Alert Rules */

pthread_t thread[NUM_OF_NF]; //threads

FILE *cyc, *fid, *num;

/****************************Internal Declarations****************************/

#define MAX_SHUTDOWN_ITERS 10

// True as long as the main thread loop should keep running
static uint8_t main_keep_running = 1;

// We'll want to shut down the TX/RX threads second so that we don't
// race the stats display to be able to print, so keep this varable separate
static uint8_t worker_keep_running = 1;

static void handle_signal(int sig);







/*******************************Worker threads********************************/

/*
 * Stats thread periodically prints per-port and per-NF stats.
 */
static void
master_thread_main(void) {
        uint16_t i;
        int shutdown_iter_count;
        const unsigned sleeptime = global_stats_sleep_time;

        RTE_LOG(INFO, APP, "Core %d: Running master thread\n", rte_lcore_id());

        if (stats_destination == ONVM_STATS_WEB) {
                RTE_LOG(INFO, APP, "ONVM stats can be viewed through the web console\n");
                RTE_LOG(INFO, APP, "\tTo activate, please run $ONVM_HOME/onvm_web/start_web_console.sh\n");
        }

        /* Initial pause so above printf is seen */
        sleep(5);

		while ( main_keep_running && sleep(sleeptime) <= sleeptime) {
				onvm_nf_check_status();
                if (stats_destination != ONVM_STATS_NONE)
                        onvm_stats_display_all(sleeptime);
        }

		
        /* Close out file references and things */
        onvm_stats_cleanup();

#ifdef RTE_LIBRTE_PDUMP
        rte_pdump_uninit();
#endif

        RTE_LOG(INFO, APP, "Core %d: Initiating shutdown sequence\n", rte_lcore_id());

        /* Stop all RX and TX threads */
        worker_keep_running = 0;

        /* Tell all NFs to stop */
        for (i = 0; i < MAX_NFS; i++) {
                if (nfs[i].info == NULL) {
                        continue;
                }
                RTE_LOG(INFO, APP, "Core %d: Notifying NF %"PRIu16" to shut down\n", rte_lcore_id(), i);
                onvm_nf_send_msg(i, MSG_STOP, NULL);
        }

        /* Wait to process all exits */
        for (shutdown_iter_count = 0;
             shutdown_iter_count < MAX_SHUTDOWN_ITERS && num_nfs > 0;
             shutdown_iter_count++) {
				
                onvm_nf_check_status();
                RTE_LOG(INFO, APP, "Core %d: Waiting for %"PRIu16" NFs to exit\n", rte_lcore_id(), num_nfs);
                sleep(sleeptime);
        }

        if (num_nfs > 0) {
                RTE_LOG(INFO, APP, "Core %d: Up to %"PRIu16" NFs may still be running and must be killed manually\n", rte_lcore_id(), num_nfs);
        }

        RTE_LOG(INFO, APP, "Core %d: Master thread done\n", rte_lcore_id());
}


/*
 * Function to receive packets from the NIC
 * and distribute them to the default service
 */
static int
rx_thread_main(void *arg) {
        uint16_t i, rx_count;
        struct rte_mbuf *pkts[PACKET_READ_SIZE];
        struct thread_info *rx = (struct thread_info*)arg;
		struct rte_ring *tx_ring;
		tx_ring = rte_ring_lookup(get_tx_queue_name(NUM_OF_NF));

		
        RTE_LOG(INFO,
                APP,
                "Core %d: Running RX thread for RX queue %d\n",
                rte_lcore_id(),
                rx->queue_id);
        for (; worker_keep_running;) {
                /* Read ports */
                for (i = 0; i < ports->num_ports; i++) {
                        rx_count = rte_eth_rx_burst(ports->id[i], rx->queue_id, \
                                        pkts, PACKET_READ_SIZE);
                        ports->rx_stats.rx[ports->id[i]] += rx_count;
                        /* Now process the NIC packets read */
                        if (likely(rx_count > 0)) {
                                // If there is no running NF, we drop all the packets of the batch.
                                if (!num_nfs) {
                                        onvm_pkt_drop_batch(pkts, rx_count);
                                } else {
										//set meta_data to packets here
                                        onvm_pkt_process_rx_batch(rx, pkts, rx_count, tx_ring);
										/*FP End*/
                                }
                        }
                }
        }

        RTE_LOG(INFO, APP, "Core %d: RX thread done\n", rte_lcore_id());

        return 0;
}


static int
tx_thread_main(void *arg) {
        struct onvm_nf *nf;
        unsigned i, tx_count;
        struct rte_mbuf *pkts[PACKET_READ_SIZE];
        struct thread_info* tx = (struct thread_info*)arg;

        if (tx->first_nf == tx->last_nf - 1) {
                RTE_LOG(INFO,
                        APP,
                        "Core %d: Running TX thread for NF %d\n",
                        rte_lcore_id(),
                        tx->first_nf);
        } else if (tx->first_nf < tx->last_nf) {
                RTE_LOG(INFO,
                        APP,
                        "Core %d: Running TX thread for NFs %d to %d\n",
                        rte_lcore_id(),
                        tx->first_nf,
                        tx->last_nf-1);
        }

        for (; worker_keep_running;) {
                /* Read packets from the NF's tx queue and process them as needed */
                for (i = tx->first_nf; i < tx->last_nf; i++) {
                        nf = &nfs[i];
                        if (!onvm_nf_is_valid(nf))
                                continue;

			/* Dequeue all packets in ring up to max possible. */
			tx_count = rte_ring_dequeue_burst(nf->tx_q, (void **) pkts, PACKET_READ_SIZE);

                        /* Now process the Client packets read */
                        if (likely(tx_count > 0)) {
                                onvm_pkt_process_tx_batch(tx, pkts, tx_count, nf);
                        }
                }

                /* Send a burst to every port */
                onvm_pkt_flush_all_ports(tx);

                /* Send a burst to every NF */
                onvm_pkt_flush_all_nfs(tx);
        }

        RTE_LOG(INFO, APP, "Core %d: TX thread done\n", rte_lcore_id());

        return 0;
}

static void
handle_signal(int sig) {
        if (sig == SIGINT || sig == SIGTERM) {
                main_keep_running = 0;
        }
}


/*******************************Main function*********************************/


int
main(int argc, char *argv[]) {
        unsigned cur_lcore, rx_lcores, tx_lcores;
        unsigned nfs_per_tx;
        unsigned i;

        /* initialise the system */

        /* Reserve ID 0 for internal manager things */
        next_instance_id = 1;
        if (init(argc, argv) < 0 )
                return -1;
        RTE_LOG(INFO, APP, "Finished Process Init.\n");

        /* clear statistics */
        onvm_stats_clear_all_nfs();

        /* Reserve n cores for: 1 Stats, 1 final Tx out, and ONVM_NUM_RX_THREADS for Rx */
        cur_lcore = rte_lcore_id();
		printf("rte_lcore_id():%d\n",rte_lcore_id());
        rx_lcores = ONVM_NUM_RX_THREADS;
        tx_lcores = rte_lcore_count() - rx_lcores - 1;
		//printf("rte_lcore_count:%d\n",rte_lcore_count());
        /* Offset cur_lcore to start assigning TX cores */
        cur_lcore += (rx_lcores-1);

        RTE_LOG(INFO, APP, "%d cores available in total\n", rte_lcore_count());
        RTE_LOG(INFO, APP, "%d cores available for handling manager RX queues\n", rx_lcores);
        RTE_LOG(INFO, APP, "%d cores available for handling TX queues\n", tx_lcores);
        RTE_LOG(INFO, APP, "%d cores available for handling stats\n", 1);

        /* Evenly assign NFs to TX threads */

        /*
         * If num NFs is zero, then we are running in dynamic NF mode.
         * We do not have a way to tell the total number of NFs running so
         * we have to calculate nfs_per_tx using MAX_NFS then.
         * We want to distribute the number of running NFs across available
         * TX threads
         */
        nfs_per_tx = ceil((float)MAX_NFS/tx_lcores);
		//printf("nfs_per_tx:%d\n",nfs_per_tx);
        // We start the system with 0 NFs active
        num_nfs = 0;

        /* Listen for ^C and docker stop so we can exit gracefully */
        signal(SIGINT, handle_signal);
        signal(SIGTERM, handle_signal);
		//printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
        for (i = 0; i < tx_lcores; i++) {
                struct thread_info *tx = calloc(1, sizeof(struct thread_info));
                tx->queue_id = i;
                tx->port_tx_buf = calloc(RTE_MAX_ETHPORTS, sizeof(struct packet_buf));
                tx->nf_rx_buf = calloc(MAX_NFS, sizeof(struct packet_buf));
                tx->first_nf = RTE_MIN(i * nfs_per_tx + 1, (unsigned)MAX_NFS);
                tx->last_nf = RTE_MIN((i+1) * nfs_per_tx + 1, (unsigned)MAX_NFS);
				printf("ID:%d,tx->first_nf:%d\n",i,tx->first_nf);
				printf("ID:%d,tx->last_nf:%d\n",i,tx->last_nf);
				
                cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);
                if (rte_eal_remote_launch(tx_thread_main, (void*)tx,  cur_lcore) == -EBUSY) {
                        RTE_LOG(ERR,
                                APP,
                                "Core %d is already busy, can't use for nf %d TX\n",
                                cur_lcore,
                                tx->first_nf);
                        return -1;
                }
        }

        /* Launch RX thread main function for each RX queue on cores */
        for (i = 0; i < rx_lcores; i++) {
                struct thread_info *rx = calloc(1, sizeof(struct thread_info));
                rx->queue_id = i;
                rx->port_tx_buf = NULL;
                rx->nf_rx_buf = calloc(MAX_NFS, sizeof(struct packet_buf));
                cur_lcore = rte_get_next_lcore(cur_lcore, 1, 1);
                if (rte_eal_remote_launch(rx_thread_main, (void *)rx, cur_lcore) == -EBUSY) {
                        RTE_LOG(ERR,
                                APP,
                                "Core %d is already busy, can't use for RX queue id %d\n",
                                cur_lcore,
                                rx->queue_id);
                        return -1;
                }
        }

        /* Master thread handles statistics and NF management */
        master_thread_main();
        return 0;
}
