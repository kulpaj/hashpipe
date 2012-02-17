/* vegas_net_thread.c
 *
 * Routine to read packets from network and put them
 * into shared memory blocks.
 */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/types.h>

#include <xgpu.h>

#include "fitshead.h"
#include "guppi_params.h"
#include "guppi_error.h"
#include "guppi_status.h"
#include "paper_databuf.h"
#include "guppi_udp.h"
#include "guppi_time.h"

#define STATUS_KEY "NETSTAT"  /* Define before guppi_threads.h */
#include "guppi_threads.h"
#include "guppi_defines.h"
#include "paper_thread.h"

#define DEBUG_NET
// put this in a header
#define N_TIME_PER_INPUT_PER_PACKET 128   
#define EXPECTED_PACKETS_PER_BLOCK  512

int paper_block_full(paper_input_databuf_t *paper_input_databuf_p, int block_i) {
    int block_full = 1;
    int i;
    for(i=0; i < N_SUB_BLOCKS_PER_INPUT_BLOCK; i++) {
    	if((uint64_t)(paper_input_databuf_p->block[block_i].header[i].chan_present[0] &
            paper_input_databuf_p->block[block_i].header[i].chan_present[1]) < (uint64_t)0xFFFFFFFFFFFFFFFF) {
		block_full = 0;
		break;
    	} 
    }
    return(block_full);
}

void clear_chan_present(paper_input_databuf_t *paper_input_databuf_p, int block_i) {
    int i;
    for(i=0; i < N_SUB_BLOCKS_PER_INPUT_BLOCK; i++) {
	paper_input_databuf_p->block[block_i].header[i].chan_present[0] = 0;
	paper_input_databuf_p->block[block_i].header[i].chan_present[1] = 0;
    }
}

int count_missing_chan(paper_input_databuf_t *paper_input_databuf_p, int block_i) {

    // Counting bits set, Brian Kernighan's way 
    int c = 0, i, j; 			// c accumulates the total bits set in v's
    for(i=0; i < N_SUB_BLOCKS_PER_INPUT_BLOCK; i++) {
	for(j=0; j < 2; j++) {
    		uint64_t v; 		// count the number of bits set in v
		v = paper_input_databuf_p->block[block_i].header[i].chan_present[j];
    		for (; v; c++) {
  			v &= v - 1; 	// clear the least significant bit set
    		}
	}
    }
    return(N_SUB_BLOCKS_PER_INPUT_BLOCK * N_CHAN - c);
}


inline int inc_block_i(block_i) {
    return((block_i + 1) % N_INPUT_BLOCKS);
}

inline int dec_block_i(block_i) {
    return(block_i == 0 ? N_INPUT_BLOCKS - 1 : block_i - 1);
}

void set_blocks_filled(paper_input_databuf_t *paper_input_databuf_p, 
		       int block_i,   
		       int block_active[],
		       int blocks_to_set) {
// The non-exceptional call of this routine will be when a block is full.
//   In this case, block_i will be two beyond the full block and blocks_to_set 
//   will be such that the final block acted upon will be the full block. 
//   This will almost always result in only the full block being set.

    int i;
    int blocks_set;
    int found_active_block = 0;

    for(i = block_i, blocks_set = 0; blocks_set < blocks_to_set; i = inc_block_i(i), blocks_set++) {
	if(found_active_block || block_active[i]) {
		// at first active block, all subsequent blocks are considered active
		found_active_block = 1;
		if(!block_active[i]) {
    			while(paper_input_databuf_wait_free(paper_input_databuf_p, block_i)) {	
				printf("target data block %d is not free!\n", block_i);
    			}
		}
		if(block_active[i] == EXPECTED_PACKETS_PER_BLOCK) {
			// good_flag = 1;
		} else { 
			// good_flag = 0;
			printf("missing %d packets for block %d at mcnt %ld  and time %ld\n", 
	                       EXPECTED_PACKETS_PER_BLOCK - block_active[i], 
	                       i, 
		               paper_input_databuf_p->block[i].header[0].mcnt,
                               time(NULL));
		}
		paper_input_databuf_set_filled(paper_input_databuf_p, i);
		block_active[i] = 0;
	}
    }
}


inline void unpack_samples(paper_input_databuf_t * paper_input_databuf_p, uint8_t * payload_p, 
			  int block_i, int sub_block_i, int time_i, int chan_i, int input_i) {

    int8_t sample, sample_real, sample_imag;

    // Unpack first input of pair
    sample      = payload_p[0];
    sample_real = sample >> 4;
    sample_imag = (int8_t)(sample << 4) >> 4;  // the cast removes the "memory" of the shifted out bits

    paper_input_databuf_p->block[block_i].sub_block[sub_block_i].time[time_i].chan[chan_i].input[input_i].real = sample_real;
    paper_input_databuf_p->block[block_i].sub_block[sub_block_i].time[time_i].chan[chan_i].input[input_i].imag = sample_imag; 

    // Unpack second input of pair
    sample      = payload_p[1];
    sample_real = sample >> 4;
    sample_imag = (int8_t)(sample << 4) >> 4;

    paper_input_databuf_p->block[block_i].sub_block[sub_block_i].time[time_i].chan[chan_i].input[input_i+1].real = sample_real;
    paper_input_databuf_p->block[block_i].sub_block[sub_block_i].time[time_i].chan[chan_i].input[input_i+1].imag = sample_imag; 
}


int write_paper_packet_to_blocks(paper_input_databuf_t *paper_input_databuf_p, struct guppi_udp_packet *p) {

    //static const int payload_size   = 128 * 64;
    static int64_t start_count      = -1;
    static int block_active[N_INPUT_BLOCKS];
    static unsigned long pkt_count;
    uint8_t * payload_p;
    uint64_t mcnt, count; 
    static uint64_t count_offset; 
    int block_i, sub_block_i, chan_group, time_i, chan_i, input_i;

    mcnt = guppi_udp_packet_mcnt(p);
    chan_group = mcnt        & 0x000000000000000F;
    chan_i     = (mcnt >> 4) & 0x000000000000007F;
    count      = mcnt >> 11;

    if(start_count < 0) {
    	if(count % N_SUB_BLOCKS_PER_INPUT_BLOCK != 0) {
		return -1;				// insist that we start on a multiple of sub_blocks/block
	}
	start_count = count;			// good to go
    }

    pkt_count++;

    // calculate block and sub_block subscripts while taking care of count rollover
    // This may not work if the rollover has occurred after a long hiatus, eg after
    // a cable disconnect/reconnect. TODO: Account for such a hiatus.
    if(count >= start_count) {
	count_offset = count - start_count;
    } else {						// we have a count rollover
	count_offset = count_offset + count + 1; 	// assumes that count is now very small, probably zero
    } 
    block_i     = (count_offset) / N_SUB_BLOCKS_PER_INPUT_BLOCK % N_INPUT_BLOCKS; 
    sub_block_i = (count_offset) % N_SUB_BLOCKS_PER_INPUT_BLOCK; 
    if(count < start_count && block_i == 0 && sub_block_i == 0) {
	start_count = count;						// reset on block,sub 0
    }

    // upon receiving the first packet for a given block, make sure it is free
    // and then clear its previous history
    if(!block_active[block_i]) {
    	while(paper_input_databuf_wait_free(paper_input_databuf_p, block_i)) {	
		printf("target data block %d is not free!\n", block_i);
    	}
	clear_chan_present(paper_input_databuf_p, block_i);
    }
    block_active[block_i] += 1;

#if 0
    // check for overrun and then initialize sub_block mcnt
    if(paper_input_databuf_p->block[block_i].header[sub_block_i].mcnt) {	// TODO: not right b/c zero is a valid mcnt!
	if(paper_input_databuf_p->block[block_i].header[sub_block_i].mcnt != count) {
		// TODO: we are about to overrun - do something
	}
    } else {
#endif
    	paper_input_databuf_p->block[block_i].header[sub_block_i].mcnt = count; 
#if 0
    }
#endif

    // update channels present
    paper_input_databuf_p->block[block_i].header[sub_block_i].chan_present[chan_i/64] |= ((uint64_t)1<<(chan_i%64));

    // unpack the packet
    for(time_i=0; time_i<N_TIME; time_i++) {
	payload_p = (uint8_t *)(p->data+8+2*time_i);
	for(input_i=0; input_i<N_INPUT; input_i+=2, payload_p+=2*N_TIME) {
		unpack_samples(paper_input_databuf_p, payload_p, block_i, sub_block_i, time_i, chan_i, input_i);
    	}
    }

    // if all channels are present, mark this block filled
    if(paper_block_full(paper_input_databuf_p, block_i)) {
#if 1
	int i;
	for(i=0;i<4;i++) printf("%d ", block_active[i]);	
	printf("\n");
#endif
	// set the full block filled, as well as any previous abandoned blocks.  Don't touch the "next" block.
	set_blocks_filled(paper_input_databuf_p, inc_block_i(inc_block_i(block_i)), block_active, N_INPUT_BLOCKS-1);
        return paper_input_databuf_p->block[block_i].header[0].mcnt;
    }

    return -1;
}

static int init(struct guppi_thread_args *args)
{
    /* Attach to status shared mem area */
    THREAD_INIT_STATUS(STATUS_KEY);

    // Get sizing parameters
    XGPUInfo xgpu_info;
    xgpuInfo(&xgpu_info);

    /* Create paper_input_databuf for output buffer */
    THREAD_INIT_DATABUF(paper_input_databuf, 4,
        xgpu_info.vecLength*sizeof(ComplexInput),
        args->output_buffer);

    // Success!
    return 0;
}

static void *run(void * _args)
{
    // Cast _args
    struct guppi_thread_args *args = (struct guppi_thread_args *)_args;

    THREAD_RUN_BEGIN(args);

    THREAD_RUN_SET_AFFINITY_PRIORITY(args);

    THREAD_RUN_ATTACH_STATUS(st);

    /* Attach to paper_input_databuf */
    THREAD_RUN_ATTACH_DATABUF(paper_input_databuf, db, args->output_buffer);

    /* Read in general parameters */
    struct guppi_params gp;
    struct sdfits pf;
    char status_buf[GUPPI_STATUS_SIZE];
    guppi_status_lock_safe(&st);
    memcpy(status_buf, st.buf, GUPPI_STATUS_SIZE);
    guppi_status_unlock_safe(&st);
    guppi_read_obs_params(status_buf, &gp, &pf);
    pthread_cleanup_push((void *)guppi_free_sdfits, &pf);

    /* Read network params */
    struct guppi_udp_params up;
    //guppi_read_net_params(status_buf, &up);
    paper_read_net_params(status_buf, &up);

    struct guppi_udp_packet p;

    /* Give all the threads a chance to start before opening network socket */
    sleep(1);

    /* Set up UDP socket */
    int rv = guppi_udp_init(&up);
    if (rv!=GUPPI_OK) {
        guppi_error("guppi_net_thread",
                "Error opening UDP socket.");
        pthread_exit(NULL);
    }
    pthread_cleanup_push((void *)guppi_udp_close, &up);

    /* Main loop */
    unsigned waiting=-1;
    signal(SIGINT,cc);
    while (run_threads) {

        /* Wait for data */
        rv = guppi_udp_wait(&up);
        if (rv!=GUPPI_OK) {
            if (rv==GUPPI_TIMEOUT) { 
                /* Set "waiting" flag */
                if (waiting!=1) {
                    guppi_status_lock_safe(&st);
                    hputs(st.buf, STATUS_KEY, "waiting");
                    guppi_status_unlock_safe(&st);
                    waiting=1;
                }
                continue; 
            } else {
                guppi_error("guppi_net_thread", 
                        "guppi_udp_wait returned error");
                perror("guppi_udp_wait");
                pthread_exit(NULL);
            }
        }
	
        /* Read packet */
        p.packet_size = recv(up.sock, p.data, GUPPI_MAX_PACKET_SIZE, 0);
        if (up.packet_size != p.packet_size) {
            if (p.packet_size != -1) {
                #ifdef DEBUG_NET
                guppi_warn("guppi_net_thread", "Incorrect pkt size");
                #endif
                continue; 
            } else {
                guppi_error("guppi_net_thread", 
                        "guppi_udp_recv returned error");
                perror("guppi_udp_recv");
                pthread_exit(NULL);
            }
        }
	
        /* Update status if needed */
        if (waiting!=0) {
            guppi_status_lock_safe(&st);
            hputs(st.buf, STATUS_KEY, "receiving");
            guppi_status_unlock_safe(&st);
            waiting=0;
        }

        // Copy packet into any blocks where it belongs.
        const int mcnt = write_paper_packet_to_blocks((paper_input_databuf_t *)db, &p);
        if(mcnt != -1) {
            guppi_status_lock_safe(&st);
            hputi4(st.buf, "NETMCNT", mcnt);
            guppi_status_unlock_safe(&st);
        }

        /* Will exit if thread has been cancelled */
        pthread_testcancel();
    }

    /* Have to close all push's */
    pthread_cleanup_pop(0); /* Closes push(guppi_udp_close) */
    pthread_cleanup_pop(0); /* Closes guppi_free_psrfits */
    THREAD_RUN_DETACH_DATAUF;
    THREAD_RUN_DETACH_STATUS;
    THREAD_RUN_END;

    return NULL;
}

static pipeline_thread_module_t module = {
    name: "paper_net_thread",
    type: PIPELINE_INPUT_THREAD,
    init: init,
    run:  run
};

static __attribute__((constructor)) void ctor()
{
  register_pipeline_thread_module(&module);
}

// vi: set ts=8 sw=4 noet :
