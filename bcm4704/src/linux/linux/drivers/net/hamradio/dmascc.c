/*
 * $Id: dmascc.c,v 1.1.1.1 2010/03/05 07:31:24 reynolds Exp $
 *
 * Driver for high-speed SCC boards (those with DMA support)
 * Copyright (C) 1997-2000 Klaus Kudielka
 *
 * S5SCC/DMA support by Janko Koleznik S52HI
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


#include <linux/module.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/if_arp.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <linux/tqueue.h>
#include <linux/version.h>
#include <asm/atomic.h>
#include <asm/bitops.h>
#include <asm/dma.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <net/ax25.h>
#include "z8530.h"


/* Linux 2.2 and 2.3 compatibility */

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,3,14)
#define net_device device
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,3,43)
#define netif_start_queue(dev) { dev->tbusy = 0; }
#define netif_stop_queue(dev) { dev->tbusy = 1; }
#define netif_wake_queue(dev) { dev->tbusy = 0; mark_bh(NET_BH); }
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,3,47)
#define netif_running(dev) (dev->flags & IFF_UP)
#endif


/* Number of buffers per channel */

#define NUM_TX_BUF      2          /* NUM_TX_BUF >= 1 (min. 2 recommended) */
#define NUM_RX_BUF      6          /* NUM_RX_BUF >= 1 (min. 2 recommended) */
#define BUF_SIZE        1576       /* BUF_SIZE >= mtu + hard_header_len */


/* Cards supported */

#define HW_PI           { "Ottawa PI", 0x300, 0x20, 0x10, 8, \
                            0, 8, 1843200, 3686400 }
#define HW_PI2          { "Ottawa PI2", 0x300, 0x20, 0x10, 8, \
			    0, 8, 3686400, 7372800 }
#define HW_TWIN         { "Gracilis PackeTwin", 0x200, 0x10, 0x10, 32, \
			    0, 4, 6144000, 6144000 }
#define HW_S5           { "S5SCC/DMA", 0x200, 0x10, 0x10, 32, \
                          0, 8, 4915200, 9830400 }

#define HARDWARE        { HW_PI, HW_PI2, HW_TWIN, HW_S5 }

#define TMR_0_HZ        25600      /* Frequency of timer 0 */

#define TYPE_PI         0
#define TYPE_PI2        1
#define TYPE_TWIN       2
#define TYPE_S5         3
#define NUM_TYPES       4

#define MAX_NUM_DEVS    32


/* SCC chips supported */

#define Z8530           0
#define Z85C30          1
#define Z85230          2

#define CHIPNAMES       { "Z8530", "Z85C30", "Z85230" }


/* I/O registers */

/* 8530 registers relative to card base */
#define SCCB_CMD        0x00
#define SCCB_DATA       0x01
#define SCCA_CMD        0x02
#define SCCA_DATA       0x03

/* 8253/8254 registers relative to card base */
#define TMR_CNT0        0x00
#define TMR_CNT1        0x01
#define TMR_CNT2        0x02
#define TMR_CTRL        0x03

/* Additional PI/PI2 registers relative to card base */
#define PI_DREQ_MASK    0x04

/* Additional PackeTwin registers relative to card base */
#define TWIN_INT_REG    0x08
#define TWIN_CLR_TMR1   0x09
#define TWIN_CLR_TMR2   0x0a
#define TWIN_SPARE_1    0x0b
#define TWIN_DMA_CFG    0x08
#define TWIN_SERIAL_CFG 0x09
#define TWIN_DMA_CLR_FF 0x0a
#define TWIN_SPARE_2    0x0b


/* PackeTwin I/O register values */

/* INT_REG */
#define TWIN_SCC_MSK       0x01
#define TWIN_TMR1_MSK      0x02
#define TWIN_TMR2_MSK      0x04
#define TWIN_INT_MSK       0x07

/* SERIAL_CFG */
#define TWIN_DTRA_ON       0x01
#define TWIN_DTRB_ON       0x02
#define TWIN_EXTCLKA       0x04
#define TWIN_EXTCLKB       0x08
#define TWIN_LOOPA_ON      0x10
#define TWIN_LOOPB_ON      0x20
#define TWIN_EI            0x80

/* DMA_CFG */
#define TWIN_DMA_HDX_T1    0x08
#define TWIN_DMA_HDX_R1    0x0a
#define TWIN_DMA_HDX_T3    0x14
#define TWIN_DMA_HDX_R3    0x16
#define TWIN_DMA_FDX_T3R1  0x1b
#define TWIN_DMA_FDX_T1R3  0x1d


/* Status values */

#define IDLE      0
#define TX_HEAD   1
#define TX_DATA   2
#define TX_PAUSE  3
#define TX_TAIL   4
#define RTS_OFF   5
#define WAIT      6
#define DCD_ON    7
#define RX_ON     8
#define DCD_OFF   9


/* Ioctls */

#define SIOCGSCCPARAM SIOCDEVPRIVATE
#define SIOCSSCCPARAM (SIOCDEVPRIVATE+1)


/* Data types */

struct scc_param {
  int pclk_hz;    /* frequency of BRG input (don't change) */
  int brg_tc;     /* BRG terminal count; BRG disabled if < 0 */
  int nrzi;       /* 0 (nrz), 1 (nrzi) */
  int clocks;     /* see dmascc_cfg documentation */
  int txdelay;    /* [1/TMR_0_HZ] */
  int txtimeout;  /* [1/HZ] */
  int txtail;     /* [1/TMR_0_HZ] */
  int waittime;   /* [1/TMR_0_HZ] */
  int slottime;   /* [1/TMR_0_HZ] */
  int persist;    /* 1 ... 256 */
  int dma;        /* -1 (disable), 0, 1, 3 */
  int txpause;    /* [1/TMR_0_HZ] */
  int rtsoff;     /* [1/TMR_0_HZ] */
  int dcdon;      /* [1/TMR_0_HZ] */
  int dcdoff;     /* [1/TMR_0_HZ] */
};

struct scc_hardware {
  char *name;
  int io_region;
  int io_delta;
  int io_size;
  int num_devs;
  int scc_offset;
  int tmr_offset;
  int tmr_hz;
  int pclk_hz;
};

struct scc_priv {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
  char name[IFNAMSIZ];
#endif
  int type;
  int chip;
  struct net_device *dev;
  struct scc_info *info;
  struct net_device_stats stats;
  int channel;
  int card_base, scc_cmd, scc_data;
  int tmr_cnt, tmr_ctrl, tmr_mode;
  struct scc_param param;
  char rx_buf[NUM_RX_BUF][BUF_SIZE];
  int rx_len[NUM_RX_BUF];
  int rx_ptr;
  struct tq_struct rx_task;
  int rx_head, rx_tail, rx_count;
  int rx_over;
  char tx_buf[NUM_TX_BUF][BUF_SIZE];
  int tx_len[NUM_TX_BUF];
  int tx_ptr;
  int tx_head, tx_tail, tx_count;
  int state;
  unsigned long tx_start;
  int rr0;
};

struct scc_info {
  int irq_used;
  int twin_serial_cfg;
  struct net_device dev[2];
  struct scc_priv priv[2];
  struct scc_info *next;
};


/* Function declarations */

int dmascc_init(void) __init;
static int setup_adapter(int card_base, int type, int n) __init;

static void write_scc(struct scc_priv *priv, int reg, int val);
static void write_scc_data(struct scc_priv *priv, int val, int fast);
static int read_scc(struct scc_priv *priv, int reg);
static int read_scc_data(struct scc_priv *priv);

static int scc_open(struct net_device *dev);
static int scc_close(struct net_device *dev);
static int scc_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd);
static int scc_send_packet(struct sk_buff *skb, struct net_device *dev);
static struct net_device_stats *scc_get_stats(struct net_device *dev);
static int scc_set_mac_address(struct net_device *dev, void *sa);

static void scc_isr(int irq, void *dev_id, struct pt_regs * regs);
static inline void z8530_isr(struct scc_info *info);
static void rx_isr(struct scc_priv *priv);
static void special_condition(struct scc_priv *priv, int rc);
static void rx_bh(void *arg);
static void tx_isr(struct scc_priv *priv);
static void es_isr(struct scc_priv *priv);
static void tm_isr(struct scc_priv *priv);

static inline void tx_on(struct scc_priv *priv);
static inline void rx_on(struct scc_priv *priv);
static inline void rx_off(struct scc_priv *priv);
static void start_timer(struct scc_priv *priv, int t, int r15);
static inline unsigned char random(void);


/* Initialization variables */

static int io[MAX_NUM_DEVS] __initdata = { 0, };
/* Beware! hw[] is also used in cleanup_module(). If __initdata also applies
   to modules, we may not declare hw[] as __initdata */
static struct scc_hardware hw[NUM_TYPES] __initdata = HARDWARE;
static char ax25_broadcast[7] __initdata =
  { 'Q'<<1, 'S'<<1, 'T'<<1, ' '<<1, ' '<<1, ' '<<1, '0'<<1 };
static char ax25_test[7] __initdata =
  { 'L'<<1, 'I'<<1, 'N'<<1, 'U'<<1, 'X'<<1, ' '<<1, '1'<<1 };


/* Global variables */

static struct scc_info *first;
static unsigned long rand;


/* Module functions */

#ifdef MODULE


MODULE_AUTHOR("Klaus Kudielka");
MODULE_DESCRIPTION("Driver for high-speed SCC boards");
MODULE_PARM(io, "1-" __MODULE_STRING(MAX_NUM_DEVS) "i");
MODULE_LICENSE("GPL");


int init_module(void) {
  return dmascc_init();
}


void cleanup_module(void) {
  int i;
  struct scc_info *info;

  while (first) {
    info = first;

    /* Unregister devices */
    for (i = 0; i < 2; i++) {
      if (info->dev[i].name)
	rtnl_lock();
	unregister_netdevice(&info->dev[i]);
	rtnl_unlock();
    }

    /* Reset board */
    if (info->priv[0].type == TYPE_TWIN)
      outb(0, info->dev[0].base_addr + TWIN_SERIAL_CFG);
    write_scc(&info->priv[0], R9, FHWRES);
    release_region(info->dev[0].base_addr,
		   hw[info->priv[0].type].io_size);

    /* Free memory */
    first = info->next;
    kfree(info);
  }
}


#else


void __init dmascc_setup(char *str, int *ints) {
   int i;

   for (i = 0; i < MAX_NUM_DEVS && i < ints[0]; i++)
      io[i] = ints[i+1];
}


#endif


/* Initialization functions */

int __init dmascc_init(void) {
  int h, i, j, n;
  int base[MAX_NUM_DEVS], tcmd[MAX_NUM_DEVS], t0[MAX_NUM_DEVS],
    t1[MAX_NUM_DEVS];
  unsigned t_val;
  unsigned long time, start[MAX_NUM_DEVS], delay[MAX_NUM_DEVS],
    counting[MAX_NUM_DEVS];

  /* Initialize random number generator */
  rand = jiffies;
  /* Cards found = 0 */
  n = 0;
  /* Warning message */
  if (!io[0]) printk("dmascc: autoprobing (dangerous)\n");

  /* Run autodetection for each card type */
  for (h = 0; h < NUM_TYPES; h++) {

    if (io[0]) {
      /* User-specified I/O address regions */
      for (i = 0; i < hw[h].num_devs; i++) base[i] = 0;
      for (i = 0; i < MAX_NUM_DEVS && io[i]; i++) {
	j = (io[i] - hw[h].io_region) / hw[h].io_delta;
	if (j >= 0 &&
	    j < hw[h].num_devs && 
	    hw[h].io_region + j * hw[h].io_delta == io[i]) {
	  base[j] = io[i];
	}
      }
    } else {
      /* Default I/O address regions */
      for (i = 0; i < hw[h].num_devs; i++) {
	base[i] = hw[h].io_region + i * hw[h].io_delta;
      }
    }

    /* Check valid I/O address regions */
    for (i = 0; i < hw[h].num_devs; i++)
      if (base[i]) {
	if (check_region(base[i], hw[h].io_size))
	  base[i] = 0;
	else {
	  tcmd[i] = base[i] + hw[h].tmr_offset + TMR_CTRL;
	  t0[i]   = base[i] + hw[h].tmr_offset + TMR_CNT0;
	  t1[i]   = base[i] + hw[h].tmr_offset + TMR_CNT1;
	}
      }

    /* Start timers */
    for (i = 0; i < hw[h].num_devs; i++)
      if (base[i]) {
	/* Timer 0: LSB+MSB, Mode 3, TMR_0_HZ */
	outb(0x36, tcmd[i]);
	outb((hw[h].tmr_hz/TMR_0_HZ) & 0xFF, t0[i]);
	outb((hw[h].tmr_hz/TMR_0_HZ) >> 8, t0[i]);
	/* Timer 1: LSB+MSB, Mode 0, HZ/10 */
	outb(0x70, tcmd[i]);
	outb((TMR_0_HZ/HZ*10) & 0xFF, t1[i]);
	outb((TMR_0_HZ/HZ*10) >> 8, t1[i]);
	start[i] = jiffies;
	delay[i] = 0;
	counting[i] = 1;
	/* Timer 2: LSB+MSB, Mode 0 */
	outb(0xb0, tcmd[i]);
      }
    time = jiffies;
    /* Wait until counter registers are loaded */
    udelay(2000000/TMR_0_HZ);

    /* Timing loop */
    while (jiffies - time < 13) {
      for (i = 0; i < hw[h].num_devs; i++)
	if (base[i] && counting[i]) {
	  /* Read back Timer 1: latch; read LSB; read MSB */
	  outb(0x40, tcmd[i]);
	  t_val = inb(t1[i]) + (inb(t1[i]) << 8);
	  /* Also check whether counter did wrap */
	  if (t_val == 0 || t_val > TMR_0_HZ/HZ*10) counting[i] = 0;
	  delay[i] = jiffies - start[i];
	}
    }

    /* Evaluate measurements */
    for (i = 0; i < hw[h].num_devs; i++)
      if (base[i]) {
	if (delay[i] >= 9 && delay[i] <= 11) {
	  /* Ok, we have found an adapter */
	  if (setup_adapter(base[i], h, n) == 0)
	    n++;
	}
      }

  } /* NUM_TYPES */

  /* If any adapter was successfully initialized, return ok */
  if (n) return 0;

  /* If no adapter found, return error */
  printk("dmascc: no adapters found\n");
  return -EIO;
}


int __init setup_adapter(int card_base, int type, int n) {
  int i, irq, chip;
  struct scc_info *info;
  struct net_device *dev;
  struct scc_priv *priv;
  unsigned long time;
  unsigned int irqs;
  int tmr_base = card_base + hw[type].tmr_offset;
  int scc_base = card_base + hw[type].scc_offset;
  char *chipnames[] = CHIPNAMES;

  /* Allocate memory */
  info = kmalloc(sizeof(struct scc_info), GFP_KERNEL | GFP_DMA);
  if (!info) {
    printk("dmascc: could not allocate memory for %s at %#3x\n",
	   hw[type].name, card_base);
    return -1;
  }

  /* Initialize what is necessary for write_scc and write_scc_data */
  memset(info, 0, sizeof(struct scc_info));
  priv = &info->priv[0];
  priv->type = type;
  priv->card_base = card_base;
  priv->scc_cmd = scc_base + SCCA_CMD;
  priv->scc_data = scc_base + SCCA_DATA;

  /* Reset SCC */
  write_scc(priv, R9, FHWRES | MIE | NV);

  /* Determine type of chip by enabling SDLC/HDLC enhancements */
  write_scc(priv, R15, SHDLCE);
  if (!read_scc(priv, R15)) {
    /* WR7' not present. This is an ordinary Z8530 SCC. */
    chip = Z8530;
  } else {
    /* Put one character in TX FIFO */
    write_scc_data(priv, 0, 0);
    if (read_scc(priv, R0) & Tx_BUF_EMP) {
      /* TX FIFO not full. This is a Z85230 ESCC with a 4-byte FIFO. */
      chip = Z85230;
    } else {
      /* TX FIFO full. This is a Z85C30 SCC with a 1-byte FIFO. */
      chip = Z85C30;
    }
  }
  write_scc(priv, R15, 0);

  /* Start IRQ auto-detection */
  sti();
  irqs = probe_irq_on();

  /* Enable interrupts */
  if (type == TYPE_TWIN) {
    outb(0, card_base + TWIN_DMA_CFG);
    inb(card_base + TWIN_CLR_TMR1);
    inb(card_base + TWIN_CLR_TMR2);
    outb((info->twin_serial_cfg = TWIN_EI), card_base + TWIN_SERIAL_CFG);
  } else {
    write_scc(priv, R15, CTSIE);
    write_scc(priv, R0, RES_EXT_INT);
    write_scc(priv, R1, EXT_INT_ENAB);
  }

  /* Start timer */
  outb(1, tmr_base + TMR_CNT1);
  outb(0, tmr_base + TMR_CNT1);

  /* Wait and detect IRQ */
  time = jiffies; while (jiffies - time < 2 + HZ / TMR_0_HZ);
  irq = probe_irq_off(irqs);

  /* Clear pending interrupt, disable interrupts */
  if (type == TYPE_TWIN) {
    inb(card_base + TWIN_CLR_TMR1);
  } else {
    write_scc(priv, R1, 0);
    write_scc(priv, R15, 0);
    write_scc(priv, R0, RES_EXT_INT);
  }

  if (irq <= 0) {
    printk("dmascc: could not find irq of %s at %#3x (irq=%d)\n",
	   hw[type].name, card_base, irq);
    kfree(info);
    return -1;
  }

  /* Set up data structures */
  for (i = 0; i < 2; i++) {
    dev = &info->dev[i];
    priv = &info->priv[i];
    priv->type = type;
    priv->chip = chip;
    priv->dev = dev;
    priv->info = info;
    priv->channel = i;
    priv->card_base = card_base;
    priv->scc_cmd = scc_base + (i ? SCCB_CMD : SCCA_CMD);
    priv->scc_data = scc_base + (i ? SCCB_DATA : SCCA_DATA);
    priv->tmr_cnt = tmr_base + (i ? TMR_CNT2 : TMR_CNT1);
    priv->tmr_ctrl = tmr_base + TMR_CTRL;
    priv->tmr_mode = i ? 0xb0 : 0x70;
    priv->param.pclk_hz = hw[type].pclk_hz;
    priv->param.brg_tc = -1;
    priv->param.clocks = TCTRxCP | RCRTxCP;
    priv->param.persist = 256;
    priv->param.dma = -1;
    priv->rx_task.routine = rx_bh;
    priv->rx_task.data = priv;
    dev->priv = priv;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,0)
    if (sizeof(dev->name) == sizeof(char *)) dev->name = priv->name;
#endif
    sprintf(dev->name, "dmascc%i", 2*n+i);
    dev->base_addr = card_base;
    dev->irq = irq;
    dev->open = scc_open;
    dev->stop = scc_close;
    dev->do_ioctl = scc_ioctl;
    dev->hard_start_xmit = scc_send_packet;
    dev->get_stats = scc_get_stats;
    dev->hard_header = ax25_encapsulate;
    dev->rebuild_header = ax25_rebuild_header;
    dev->set_mac_address = scc_set_mac_address;
    dev->type = ARPHRD_AX25;
    dev->hard_header_len = 73;
    dev->mtu = 1500;
    dev->addr_len = 7;
    dev->tx_queue_len = 64;
    memcpy(dev->broadcast, ax25_broadcast, 7);
    memcpy(dev->dev_addr, ax25_test, 7);
    rtnl_lock();
    if (register_netdevice(dev)) {
      printk("dmascc: could not register %s\n", dev->name);
    }
    rtnl_unlock();
  }

  request_region(card_base, hw[type].io_size, "dmascc");

  info->next = first;
  first = info;
  printk("dmascc: found %s (%s) at %#3x, irq %d\n", hw[type].name,
	 chipnames[chip], card_base, irq);
  return 0;
}


/* Driver functions */

static void write_scc(struct scc_priv *priv, int reg, int val) {
  unsigned long flags;
  switch (priv->type) {
  case TYPE_S5:
    if (reg) outb(reg, priv->scc_cmd);
    outb(val, priv->scc_cmd);
    return;
  case TYPE_TWIN:
    if (reg) outb_p(reg, priv->scc_cmd);
    outb_p(val, priv->scc_cmd);
    return;
  default:
    save_flags(flags);
    cli();
    outb_p(0, priv->card_base + PI_DREQ_MASK);
    if (reg) outb_p(reg, priv->scc_cmd);
    outb_p(val, priv->scc_cmd);
    outb(1, priv->card_base + PI_DREQ_MASK);
    restore_flags(flags);
    return;
  }
}


static void write_scc_data(struct scc_priv *priv, int val, int fast) {
  unsigned long flags;
  switch (priv->type) {
  case TYPE_S5:
    outb(val, priv->scc_data);
    return;
  case TYPE_TWIN:
    outb_p(val, priv->scc_data);
    return;
  default:
    if (fast) outb_p(val, priv->scc_data);
    else {
      save_flags(flags);
      cli();
      outb_p(0, priv->card_base + PI_DREQ_MASK);
      outb_p(val, priv->scc_data);
      outb(1, priv->card_base + PI_DREQ_MASK);
      restore_flags(flags);
    }
    return;
  }
}


static int read_scc(struct scc_priv *priv, int reg) {
  int rc;
  unsigned long flags;
  switch (priv->type) {
  case TYPE_S5:
    if (reg) outb(reg, priv->scc_cmd);
    return inb(priv->scc_cmd);
  case TYPE_TWIN:
    if (reg) outb_p(reg, priv->scc_cmd);
    return inb_p(priv->scc_cmd);
  default:
    save_flags(flags);
    cli();
    outb_p(0, priv->card_base + PI_DREQ_MASK);
    if (reg) outb_p(reg, priv->scc_cmd);
    rc = inb_p(priv->scc_cmd);
    outb(1, priv->card_base + PI_DREQ_MASK);
    restore_flags(flags);
    return rc;
  }
}


static int read_scc_data(struct scc_priv *priv) {
  int rc;
  unsigned long flags;
  switch (priv->type) {
  case TYPE_S5:
    return inb(priv->scc_data);
  case TYPE_TWIN:
    return inb_p(priv->scc_data);
  default:
    save_flags(flags);
    cli();
    outb_p(0, priv->card_base + PI_DREQ_MASK);
    rc = inb_p(priv->scc_data);
    outb(1, priv->card_base + PI_DREQ_MASK);
    restore_flags(flags);
    return rc;
  }
}


static int scc_open(struct net_device *dev) {
  struct scc_priv *priv = dev->priv;
  struct scc_info *info = priv->info;
  int card_base = priv->card_base;

  MOD_INC_USE_COUNT;

  /* Request IRQ if not already used by other channel */
  if (!info->irq_used) {
    if (request_irq(dev->irq, scc_isr, 0, "dmascc", info)) {
      MOD_DEC_USE_COUNT;
      return -EAGAIN;
    }
  }
  info->irq_used++;

  /* Request DMA if required */
  if (priv->param.dma >= 0) {
    if (request_dma(priv->param.dma, "dmascc")) {
      if (--info->irq_used == 0) free_irq(dev->irq, info);
      MOD_DEC_USE_COUNT;
      return -EAGAIN;
    } else {
      unsigned long flags = claim_dma_lock();
      clear_dma_ff(priv->param.dma);
      release_dma_lock(flags);
    }
  }

  /* Initialize local variables */
  priv->rx_ptr = 0;
  priv->rx_over = 0;
  priv->rx_head = priv->rx_tail = priv->rx_count = 0;
  priv->state = IDLE;
  priv->tx_head = priv->tx_tail = priv->tx_count = 0;
  priv->tx_ptr = 0;

  /* Reset channel */
  write_scc(priv, R9, (priv->channel ? CHRB : CHRA) | MIE | NV);
  /* X1 clock, SDLC mode */
  write_scc(priv, R4, SDLC | X1CLK);
  /* DMA */
  write_scc(priv, R1, EXT_INT_ENAB | WT_FN_RDYFN);
  /* 8 bit RX char, RX disable */
  write_scc(priv, R3, Rx8);
  /* 8 bit TX char, TX disable */
  write_scc(priv, R5, Tx8);
  /* SDLC address field */
  write_scc(priv, R6, 0);
  /* SDLC flag */
  write_scc(priv, R7, FLAG);
  switch (priv->chip) {
  case Z85C30:
    /* Select WR7' */
    write_scc(priv, R15, SHDLCE);
    /* Auto EOM reset */
    write_scc(priv, R7, AUTOEOM);
    write_scc(priv, R15, 0);
    break;
  case Z85230:
    /* Select WR7' */
    write_scc(priv, R15, SHDLCE);
    /* The following bits are set (see 2.5.2.1):
       - Automatic EOM reset
       - Interrupt request if RX FIFO is half full
         This bit should be ignored in DMA mode (according to the
         documentation), but actually isn't. The receiver doesn't work if
         it is set. Thus, we have to clear it in DMA mode.
       - Interrupt/DMA request if TX FIFO is completely empty
         a) If set, the ESCC behaves as if it had no TX FIFO (Z85C30
            compatibility).
         b) If cleared, DMA requests may follow each other very quickly,
            filling up the TX FIFO.
            Advantage: TX works even in case of high bus latency.
            Disadvantage: Edge-triggered DMA request circuitry may miss
                          a request. No more data is delivered, resulting
                          in a TX FIFO underrun.
         Both PI2 and S5SCC/DMA seem to work fine with TXFIFOE cleared.
         The PackeTwin doesn't. I don't know about the PI, but let's
	 assume it behaves like the PI2.
    */
    if (priv->param.dma >= 0) {
      if (priv->type == TYPE_TWIN) write_scc(priv, R7, AUTOEOM | TXFIFOE);
      else write_scc(priv, R7, AUTOEOM);
    } else {
      write_scc(priv, R7, AUTOEOM | RXFIFOH);
    }
    write_scc(priv, R15, 0);
    break;
  }
  /* Preset CRC, NRZ(I) encoding */
  write_scc(priv, R10, CRCPS | (priv->param.nrzi ? NRZI : NRZ));

  /* Configure baud rate generator */
  if (priv->param.brg_tc >= 0) {
    /* Program BR generator */
    write_scc(priv, R12, priv->param.brg_tc & 0xFF);
    write_scc(priv, R13, (priv->param.brg_tc>>8) & 0xFF);
    /* BRG source = SYS CLK; enable BRG; DTR REQ function (required by
       PackeTwin, not connected on the PI2); set DPLL source to BRG */
    write_scc(priv, R14, SSBR | DTRREQ | BRSRC | BRENABL);
    /* Enable DPLL */
    write_scc(priv, R14, SEARCH | DTRREQ | BRSRC | BRENABL);
  } else {
    /* Disable BR generator */
    write_scc(priv, R14, DTRREQ | BRSRC);
  }

  /* Configure clocks */
  if (priv->type == TYPE_TWIN) {
    /* Disable external TX clock receiver */
    outb((info->twin_serial_cfg &=
	    ~(priv->channel ? TWIN_EXTCLKB : TWIN_EXTCLKA)), 
	   card_base + TWIN_SERIAL_CFG);
  }
  write_scc(priv, R11, priv->param.clocks);
  if ((priv->type == TYPE_TWIN) && !(priv->param.clocks & TRxCOI)) {
    /* Enable external TX clock receiver */
    outb((info->twin_serial_cfg |=
	    (priv->channel ? TWIN_EXTCLKB : TWIN_EXTCLKA)),
	   card_base + TWIN_SERIAL_CFG);
  }

  /* Configure PackeTwin */
  if (priv->type == TYPE_TWIN) {
    /* Assert DTR, enable interrupts */
    outb((info->twin_serial_cfg |= TWIN_EI |
	    (priv->channel ? TWIN_DTRB_ON : TWIN_DTRA_ON)),
	   card_base + TWIN_SERIAL_CFG);
  }

  /* Read current status */
  priv->rr0 = read_scc(priv, R0);
  /* Enable DCD interrupt */
  write_scc(priv, R15, DCDIE);

  netif_start_queue(dev);

  return 0;
}


static int scc_close(struct net_device *dev) {
  struct scc_priv *priv = dev->priv;
  struct scc_info *info = priv->info;
  int card_base = priv->card_base;

  netif_stop_queue(dev);

  if (priv->type == TYPE_TWIN) {
    /* Drop DTR */
    outb((info->twin_serial_cfg &=
	    (priv->channel ? ~TWIN_DTRB_ON : ~TWIN_DTRA_ON)),
	   card_base + TWIN_SERIAL_CFG);
  }

  /* Reset channel, free DMA and IRQ */
  write_scc(priv, R9, (priv->channel ? CHRB : CHRA) | MIE | NV);
  if (priv->param.dma >= 0) {
    if (priv->type == TYPE_TWIN) outb(0, card_base + TWIN_DMA_CFG);
    free_dma(priv->param.dma);
  }
  if (--info->irq_used == 0) free_irq(dev->irq, info);

  MOD_DEC_USE_COUNT;
  return 0;
}


static int scc_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd) {
  struct scc_priv *priv = dev->priv;
  
  switch (cmd) {
  case SIOCGSCCPARAM:
    if (copy_to_user(ifr->ifr_data, &priv->param, sizeof(struct scc_param)))
      return -EFAULT;
    return 0;
  case SIOCSSCCPARAM:
    if (!capable(CAP_NET_ADMIN)) return -EPERM;
    if (netif_running(dev)) return -EAGAIN;
    if (copy_from_user(&priv->param, ifr->ifr_data, sizeof(struct scc_param)))
      return -EFAULT;
    return 0;
  default:
    return -EINVAL;
  }
}


static int scc_send_packet(struct sk_buff *skb, struct net_device *dev) {
  struct scc_priv *priv = dev->priv;
  unsigned long flags;
  int i;

  /* Temporarily stop the scheduler feeding us packets */
  netif_stop_queue(dev);

  /* Transfer data to DMA buffer */
  i = priv->tx_head;
  memcpy(priv->tx_buf[i], skb->data+1, skb->len-1);
  priv->tx_len[i] = skb->len-1;

  /* Clear interrupts while we touch our circular buffers */
  save_flags(flags);
  cli();

  /* Move the ring buffer's head */
  priv->tx_head = (i + 1) % NUM_TX_BUF;
  priv->tx_count++;

  /* If we just filled up the last buffer, leave queue stopped.
     The higher layers must wait until we have a DMA buffer
     to accept the data. */
  if (priv->tx_count < NUM_TX_BUF) netif_wake_queue(dev);

  /* Set new TX state */
  if (priv->state == IDLE) {
    /* Assert RTS, start timer */
    priv->state = TX_HEAD;
    priv->tx_start = jiffies;
    write_scc(priv, R5, TxCRC_ENAB | RTS | TxENAB | Tx8);
    write_scc(priv, R15, 0);
    start_timer(priv, priv->param.txdelay, 0);
  }

  /* Turn interrupts back on and free buffer */
  restore_flags(flags);
  dev_kfree_skb(skb);

  return 0;
}


static struct net_device_stats *scc_get_stats(struct net_device *dev) {
  struct scc_priv *priv = dev->priv;

  return &priv->stats;
}


static int scc_set_mac_address(struct net_device *dev, void *sa) {
  memcpy(dev->dev_addr, ((struct sockaddr *)sa)->sa_data, dev->addr_len);
  return 0;
}


static void scc_isr(int irq, void *dev_id, struct pt_regs * regs) {
  struct scc_info *info = dev_id;

  /* At this point interrupts are enabled, and the interrupt under service
     is already acknowledged, but masked off.

     Interrupt processing: We loop until we know that the IRQ line is
     low. If another positive edge occurs afterwards during the ISR,
     another interrupt will be triggered by the interrupt controller
     as soon as the IRQ level is enabled again (see asm/irq.h).

     Bottom-half handlers will be processed after scc_isr(). This is
     important, since we only have small ringbuffers and want new data
     to be fetched/delivered immediately. */

  if (info->priv[0].type == TYPE_TWIN) {
    int is, card_base = info->priv[0].card_base;
    while ((is = ~inb(card_base + TWIN_INT_REG)) &
	   TWIN_INT_MSK) {
      if (is & TWIN_SCC_MSK) {
	z8530_isr(info);
      } else if (is & TWIN_TMR1_MSK) {
	inb(card_base + TWIN_CLR_TMR1);
	tm_isr(&info->priv[0]);
      } else {
	inb(card_base + TWIN_CLR_TMR2);
	tm_isr(&info->priv[1]);
      }
    }
  } else z8530_isr(info);
}


static inline void z8530_isr(struct scc_info *info) {
  int is, i = 100;

  while ((is = read_scc(&info->priv[0], R3)) && i--) {
    if (is & CHARxIP) {
      rx_isr(&info->priv[0]);
    } else if (is & CHATxIP) {
      tx_isr(&info->priv[0]);
    } else if (is & CHAEXT) {
      es_isr(&info->priv[0]);
    } else if (is & CHBRxIP) {
      rx_isr(&info->priv[1]);
    } else if (is & CHBTxIP) {
      tx_isr(&info->priv[1]);
    } else {
      es_isr(&info->priv[1]);
    }
    write_scc(&info->priv[0], R0, RES_H_IUS);
    i++;
  }
  if (i < 0) {
    printk("dmascc: stuck in ISR with RR3=0x%02x.\n", is);
  }
  /* Ok, no interrupts pending from this 8530. The INT line should
     be inactive now. */
}


static void rx_isr(struct scc_priv *priv) {
  if (priv->param.dma >= 0) {
    /* Check special condition and perform error reset. See 2.4.7.5. */
    special_condition(priv, read_scc(priv, R1));
    write_scc(priv, R0, ERR_RES);
  } else {
    /* Check special condition for each character. Error reset not necessary.
       Same algorithm for SCC and ESCC. See 2.4.7.1 and 2.4.7.4. */
    int rc;
    while (read_scc(priv, R0) & Rx_CH_AV) {
      rc = read_scc(priv, R1);
      if (priv->rx_ptr < BUF_SIZE)
	priv->rx_buf[priv->rx_head][priv->rx_ptr++] =
	  read_scc_data(priv);
      else {
	priv->rx_over = 2;
	read_scc_data(priv);
      }
      special_condition(priv, rc);
    }
  }
}


static void special_condition(struct scc_priv *priv, int rc) {
  int cb;
  unsigned long flags;

  /* See Figure 2-15. Only overrun and EOF need to be checked. */
  
  if (rc & Rx_OVR) {
    /* Receiver overrun */
    priv->rx_over = 1;
    if (priv->param.dma < 0) write_scc(priv, R0, ERR_RES);
  } else if (rc & END_FR) {
    /* End of frame. Get byte count */
    if (priv->param.dma >= 0) {
      flags = claim_dma_lock();
      cb = BUF_SIZE - get_dma_residue(priv->param.dma) - 2;
      release_dma_lock(flags);
    } else {
      cb = priv->rx_ptr - 2;
    }
    if (priv->rx_over) {
      /* We had an overrun */
      priv->stats.rx_errors++;
      if (priv->rx_over == 2) priv->stats.rx_length_errors++;
      else priv->stats.rx_fifo_errors++;
      priv->rx_over = 0;
    } else if (rc & CRC_ERR) {
      /* Count invalid CRC only if packet length >= minimum */
      if (cb >= 15) {
	priv->stats.rx_errors++;
	priv->stats.rx_crc_errors++;
      }
    } else {
      if (cb >= 15) {
	if (priv->rx_count < NUM_RX_BUF - 1) {
	  /* Put good frame in FIFO */
	  priv->rx_len[priv->rx_head] = cb;
	  priv->rx_head = (priv->rx_head + 1) % NUM_RX_BUF;
	  priv->rx_count++;
	  /* Mark bottom half handler */
	  queue_task(&priv->rx_task, &tq_immediate);
	  mark_bh(IMMEDIATE_BH);
	} else {
	  priv->stats.rx_errors++;
	  priv->stats.rx_over_errors++;
	}
      }
    }
    /* Get ready for new frame */
    if (priv->param.dma >= 0) {
      flags = claim_dma_lock();
      set_dma_addr(priv->param.dma, (int) priv->rx_buf[priv->rx_head]);
      set_dma_count(priv->param.dma, BUF_SIZE);
      release_dma_lock(flags);
    } else {
      priv->rx_ptr = 0;
    }
  }
}


static void rx_bh(void *arg) {
  struct scc_priv *priv = arg;
  int i = priv->rx_tail;
  int cb;
  unsigned long flags;
  struct sk_buff *skb;
  unsigned char *data;

  save_flags(flags);
  cli();

  while (priv->rx_count) {
    restore_flags(flags);
    cb = priv->rx_len[i];
    /* Allocate buffer */
    skb = dev_alloc_skb(cb+1);
    if (skb == NULL) {
      /* Drop packet */
      priv->stats.rx_dropped++;
    } else {
      /* Fill buffer */
      data = skb_put(skb, cb+1);
      data[0] = 0;
      memcpy(&data[1], priv->rx_buf[i], cb);
      skb->dev = priv->dev;
      skb->protocol = ntohs(ETH_P_AX25);
      skb->mac.raw = skb->data;
      netif_rx(skb);
      priv->stats.rx_packets++;
      priv->stats.rx_bytes += cb;
    }
    save_flags(flags);
    cli();
    /* Move tail */
    priv->rx_tail = i = (i + 1) % NUM_RX_BUF;
    priv->rx_count--;
  }

  restore_flags(flags);
}


static void tx_isr(struct scc_priv *priv) {
  int i = priv->tx_tail, p = priv->tx_ptr;

  /* Suspend TX interrupts if we don't want to send anything.
     See Figure 2-22. */
  if (p ==  priv->tx_len[i]) {
    write_scc(priv, R0, RES_Tx_P);
    return;
  }

  /* Write characters */
  while ((read_scc(priv, R0) & Tx_BUF_EMP) && p < priv->tx_len[i]) {
    write_scc_data(priv, priv->tx_buf[i][p++], 0);
  }

  /* Reset EOM latch of Z8530 */
  if (!priv->tx_ptr && p && priv->chip == Z8530)
    write_scc(priv, R0, RES_EOM_L);

  priv->tx_ptr = p;
}


static void es_isr(struct scc_priv *priv) {
  int i, rr0, drr0, res;
  unsigned long flags;

  /* Read status, reset interrupt bit (open latches) */
  rr0 = read_scc(priv, R0);
  write_scc(priv, R0, RES_EXT_INT);
  drr0 = priv->rr0 ^ rr0;
  priv->rr0 = rr0;

  /* Transmit underrun (2.4.9.6). We can't check the TxEOM flag, since
     it might have already been cleared again by AUTOEOM. */
  if (priv->state == TX_DATA) {
    /* Get remaining bytes */
    i = priv->tx_tail;
    if (priv->param.dma >= 0) {
      disable_dma(priv->param.dma);
      flags = claim_dma_lock();
      res = get_dma_residue(priv->param.dma);
      release_dma_lock(flags);
    } else {
      res = priv->tx_len[i] - priv->tx_ptr;
      priv->tx_ptr = 0;
    }
    /* Disable DREQ / TX interrupt */
    if (priv->param.dma >= 0 && priv->type == TYPE_TWIN)
      outb(0, priv->card_base + TWIN_DMA_CFG);
    else
      write_scc(priv, R1, EXT_INT_ENAB | WT_FN_RDYFN);
    if (res) {
      /* Update packet statistics */
      priv->stats.tx_errors++;
      priv->stats.tx_fifo_errors++;
      /* Other underrun interrupts may already be waiting */
      write_scc(priv, R0, RES_EXT_INT);
      write_scc(priv, R0, RES_EXT_INT);
    } else {
      /* Update packet statistics */
      priv->stats.tx_packets++;
      priv->stats.tx_bytes += priv->tx_len[i];
      /* Remove frame from FIFO */
      priv->tx_tail = (i + 1) % NUM_TX_BUF;
      priv->tx_count--;
      /* Inform upper layers */
      netif_wake_queue(priv->dev);
    }
    /* Switch state */
    write_scc(priv, R15, 0);
    if (priv->tx_count &&
	(jiffies - priv->tx_start) < priv->param.txtimeout) {
      priv->state = TX_PAUSE;
      start_timer(priv, priv->param.txpause, 0);
    } else {
      priv->state = TX_TAIL;
      start_timer(priv, priv->param.txtail, 0);
    }
  }

  /* DCD transition */
  if (drr0 & DCD) {
    if (rr0 & DCD) {
      switch (priv->state) {
      case IDLE:
      case WAIT:
	priv->state = DCD_ON;
	write_scc(priv, R15, 0);
	start_timer(priv, priv->param.dcdon, 0);
      }
    } else {
      switch (priv->state) {
      case RX_ON:
	rx_off(priv);
	priv->state = DCD_OFF;
	write_scc(priv, R15, 0);
	start_timer(priv, priv->param.dcdoff, 0);
      }
    }
  }

  /* CTS transition */
  if ((drr0 & CTS) && (~rr0 & CTS) && priv->type != TYPE_TWIN)
    tm_isr(priv);

}


static void tm_isr(struct scc_priv *priv) {
  switch (priv->state) {
  case TX_HEAD:
  case TX_PAUSE:
    tx_on(priv);
    priv->state = TX_DATA;
    break;
  case TX_TAIL:
    write_scc(priv, R5, TxCRC_ENAB | Tx8);
    priv->state = RTS_OFF;
    if (priv->type != TYPE_TWIN) write_scc(priv, R15, 0);
    start_timer(priv, priv->param.rtsoff, 0);
    break;
  case RTS_OFF:
    write_scc(priv, R15, DCDIE);
    priv->rr0 = read_scc(priv, R0);
    if (priv->rr0 & DCD) {
      priv->stats.collisions++;
      rx_on(priv);
      priv->state = RX_ON;
    } else {
      priv->state = WAIT;
      start_timer(priv, priv->param.waittime, DCDIE);
    }
    break;
  case WAIT:
    if (priv->tx_count) {
      priv->state = TX_HEAD;
      priv->tx_start = jiffies;
      write_scc(priv, R5, TxCRC_ENAB | RTS | TxENAB | Tx8);
      write_scc(priv, R15, 0);
      start_timer(priv, priv->param.txdelay, 0);
    } else {
      priv->state = IDLE;
      if (priv->type != TYPE_TWIN) write_scc(priv, R15, DCDIE);
    }
    break;
  case DCD_ON:
  case DCD_OFF:
    write_scc(priv, R15, DCDIE);
    priv->rr0 = read_scc(priv, R0);
    if (priv->rr0 & DCD) {
      rx_on(priv);
      priv->state = RX_ON;
    } else {
      priv->state = WAIT;
      start_timer(priv,
		  random()/priv->param.persist*priv->param.slottime,
		  DCDIE);
    }
    break;
  }
}


static inline void tx_on(struct scc_priv *priv) {
  int i, n;
  unsigned long flags;

  if (priv->param.dma >= 0) {
    n = (priv->chip == Z85230) ? 3 : 1;
    /* Program DMA controller */
    flags = claim_dma_lock();
    set_dma_mode(priv->param.dma, DMA_MODE_WRITE);
    set_dma_addr(priv->param.dma, (int) priv->tx_buf[priv->tx_tail]+n);
    set_dma_count(priv->param.dma, priv->tx_len[priv->tx_tail]-n);
    release_dma_lock(flags);
    /* Enable TX underrun interrupt */
    write_scc(priv, R15, TxUIE);
    /* Configure DREQ */
    if (priv->type == TYPE_TWIN)
      outb((priv->param.dma == 1) ? TWIN_DMA_HDX_T1 : TWIN_DMA_HDX_T3,
	   priv->card_base + TWIN_DMA_CFG);
    else
      write_scc(priv, R1, EXT_INT_ENAB | WT_FN_RDYFN | WT_RDY_ENAB);
    /* Write first byte(s) */
    save_flags(flags);
    cli();
    for (i = 0; i < n; i++)
      write_scc_data(priv, priv->tx_buf[priv->tx_tail][i], 1);
    enable_dma(priv->param.dma);
    restore_flags(flags);
  } else {
    write_scc(priv, R15, TxUIE);
    write_scc(priv, R1, EXT_INT_ENAB | WT_FN_RDYFN | TxINT_ENAB);
    tx_isr(priv);
  }
  /* Reset EOM latch if we do not have the AUTOEOM feature */
  if (priv->chip == Z8530) write_scc(priv, R0, RES_EOM_L);
}


static inline void rx_on(struct scc_priv *priv) {
  unsigned long flags;

  /* Clear RX FIFO */
  while (read_scc(priv, R0) & Rx_CH_AV) read_scc_data(priv);
  priv->rx_over = 0;
  if (priv->param.dma >= 0) {
    /* Program DMA controller */
    flags = claim_dma_lock();
    set_dma_mode(priv->param.dma, DMA_MODE_READ);
    set_dma_addr(priv->param.dma, (int) priv->rx_buf[priv->rx_head]);
    set_dma_count(priv->param.dma, BUF_SIZE);
    release_dma_lock(flags);
    enable_dma(priv->param.dma);
    /* Configure PackeTwin DMA */
    if (priv->type == TYPE_TWIN) {
      outb((priv->param.dma == 1) ? TWIN_DMA_HDX_R1 : TWIN_DMA_HDX_R3,
	   priv->card_base + TWIN_DMA_CFG);
    }
    /* Sp. cond. intr. only, ext int enable, RX DMA enable */
    write_scc(priv, R1, EXT_INT_ENAB | INT_ERR_Rx |
	      WT_RDY_RT | WT_FN_RDYFN | WT_RDY_ENAB);
  } else {
    /* Reset current frame */
    priv->rx_ptr = 0;
    /* Intr. on all Rx characters and Sp. cond., ext int enable */
    write_scc(priv, R1, EXT_INT_ENAB | INT_ALL_Rx | WT_RDY_RT |
	      WT_FN_RDYFN);
  }
  write_scc(priv, R0, ERR_RES);
  write_scc(priv, R3, RxENABLE | Rx8 | RxCRC_ENAB);
}


static inline void rx_off(struct scc_priv *priv) {
  /* Disable receiver */
  write_scc(priv, R3, Rx8);
  /* Disable DREQ / RX interrupt */
  if (priv->param.dma >= 0 && priv->type == TYPE_TWIN)
    outb(0, priv->card_base + TWIN_DMA_CFG);
  else
    write_scc(priv, R1, EXT_INT_ENAB | WT_FN_RDYFN);
  /* Disable DMA */
  if (priv->param.dma >= 0) disable_dma(priv->param.dma);
}


static void start_timer(struct scc_priv *priv, int t, int r15) {
  unsigned long flags;

  outb(priv->tmr_mode, priv->tmr_ctrl);
  if (t == 0) {
    tm_isr(priv);
  } else if (t > 0) {
    save_flags(flags);
    cli();
    outb(t & 0xFF, priv->tmr_cnt);
    outb((t >> 8) & 0xFF, priv->tmr_cnt);
    if (priv->type != TYPE_TWIN) {
      write_scc(priv, R15, r15 | CTSIE);
      priv->rr0 |= CTS;
    }
    restore_flags(flags);
  }
}


static inline unsigned char random(void) {
  /* See "Numerical Recipes in C", second edition, p. 284 */
  rand = rand * 1664525L + 1013904223L;
  return (unsigned char) (rand >> 24);
}

