/*
 * $Id: synclinkmp.c,v 1.1.1.1 2010/03/05 07:31:28 reynolds Exp $
 *
 * Device driver for Microgate SyncLink Multiport
 * high speed multiprotocol serial adapter.
 *
 * written by Paul Fulghum for Microgate Corporation
 * paulkf@microgate.com
 *
 * Microgate and SyncLink are trademarks of Microgate Corporation
 *
 * Derived from serial.c written by Theodore Ts'o and Linus Torvalds
 * This code is released under the GNU General Public License (GPL)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define VERSION(ver,rel,seq) (((ver)<<16) | ((rel)<<8) | (seq))
#if defined(__i386__)
#  define BREAKPOINT() asm("   int $3");
#else
#  define BREAKPOINT() { }
#endif

#define MAX_DEVICES 12

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/vmalloc.h>
#include <linux/init.h>
#include <asm/serial.h>
#include <linux/delay.h>
#include <linux/ioctl.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/dma.h>
#include <asm/bitops.h>
#include <asm/types.h>
#include <linux/termios.h>
#include <linux/tqueue.h>

#ifdef CONFIG_SYNCLINK_SYNCPPP_MODULE
#define CONFIG_SYNCLINK_SYNCPPP 1
#endif

#ifdef CONFIG_SYNCLINK_SYNCPPP
#include <net/syncppp.h>
#endif

#include <asm/segment.h>
#define GET_USER(error,value,addr) error = get_user(value,addr)
#define COPY_FROM_USER(error,dest,src,size) error = copy_from_user(dest,src,size) ? -EFAULT : 0
#define PUT_USER(error,value,addr) error = put_user(value,addr)
#define COPY_TO_USER(error,dest,src,size) error = copy_to_user(dest,src,size) ? -EFAULT : 0

#include <asm/uaccess.h>

#include "linux/synclink.h"

static MGSL_PARAMS default_params = {
	MGSL_MODE_HDLC,			/* unsigned long mode */
	0,				/* unsigned char loopback; */
	HDLC_FLAG_UNDERRUN_ABORT15,	/* unsigned short flags; */
	HDLC_ENCODING_NRZI_SPACE,	/* unsigned char encoding; */
	0,				/* unsigned long clock_speed; */
	0xff,				/* unsigned char addr_filter; */
	HDLC_CRC_16_CCITT,		/* unsigned short crc_type; */
	HDLC_PREAMBLE_LENGTH_8BITS,	/* unsigned char preamble_length; */
	HDLC_PREAMBLE_PATTERN_NONE,	/* unsigned char preamble; */
	9600,				/* unsigned long data_rate; */
	8,				/* unsigned char data_bits; */
	1,				/* unsigned char stop_bits; */
	ASYNC_PARITY_NONE		/* unsigned char parity; */
};

/* size in bytes of DMA data buffers */
#define SCABUFSIZE 	1024
#define SCA_MEM_SIZE	0x40000
#define SCA_BASE_SIZE   512
#define SCA_REG_SIZE    16
#define SCA_MAX_PORTS   4
#define SCAMAXDESC 	128

#define	BUFFERLISTSIZE	4096

/* SCA-I style DMA buffer descriptor */
typedef struct _SCADESC
{
	u16	next;		/* lower l6 bits of next descriptor addr */
	u16	buf_ptr;	/* lower 16 bits of buffer addr */
	u8	buf_base;	/* upper 8 bits of buffer addr */
	u8	pad1;
	u16	length;		/* length of buffer */
	u8	status;		/* status of buffer */
	u8	pad2;
} SCADESC, *PSCADESC;

typedef struct _SCADESC_EX
{
	/* device driver bookkeeping section */
	char 	*virt_addr;    	/* virtual address of data buffer */
	u16	phys_entry;	/* lower 16-bits of physical address of this descriptor */
} SCADESC_EX, *PSCADESC_EX;

/* The queue of BH actions to be performed */

#define BH_RECEIVE  1
#define BH_TRANSMIT 2
#define BH_STATUS   4

#define IO_PIN_SHUTDOWN_LIMIT 100

#define RELEVANT_IFLAG(iflag) (iflag & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))

struct	_input_signal_events {
	int	ri_up;
	int	ri_down;
	int	dsr_up;
	int	dsr_down;
	int	dcd_up;
	int	dcd_down;
	int	cts_up;
	int	cts_down;
};

/*
 * Device instance data structure
 */
typedef struct _synclinkmp_info {
	void *if_ptr;				/* General purpose pointer (used by SPPP) */
	int			magic;
	int			flags;
	int			count;		/* count of opens */
	int			line;
	unsigned short		close_delay;
	unsigned short		closing_wait;	/* time to wait before closing */

	struct mgsl_icount	icount;

	struct termios		normal_termios;
	struct termios		callout_termios;

	struct tty_struct 	*tty;
	int			timeout;
	int			x_char;		/* xon/xoff character */
	int			blocked_open;	/* # of blocked opens */
	long			session;	/* Session of opening process */
	long			pgrp;		/* pgrp of opening process */
	u16			read_status_mask1;  /* break detection (SR1 indications) */
	u16			read_status_mask2;  /* parity/framing/overun (SR2 indications) */
	unsigned char 		ignore_status_mask1;  /* break detection (SR1 indications) */
	unsigned char		ignore_status_mask2;  /* parity/framing/overun (SR2 indications) */
	unsigned char 		*tx_buf;
	int			tx_put;
	int			tx_get;
	int			tx_count;

	wait_queue_head_t	open_wait;
	wait_queue_head_t	close_wait;

	wait_queue_head_t	status_event_wait_q;
	wait_queue_head_t	event_wait_q;
	struct timer_list	tx_timer;	/* HDLC transmit timeout timer */
	struct _synclinkmp_info	*next_device;	/* device list link */
	struct timer_list	status_timer;	/* input signal status check timer */

	spinlock_t lock;		/* spinlock for synchronizing with ISR */
	struct tq_struct task;	 		/* task structure for scheduling bh */

	u32 max_frame_size;			/* as set by device config */

	u32 pending_bh;

	int bh_running;				/* Protection from multiple */
	int isr_overflow;
	int bh_requested;

	int dcd_chkcount;			/* check counts to prevent */
	int cts_chkcount;			/* too many IRQs if a signal */
	int dsr_chkcount;			/* is floating */
	int ri_chkcount;

	char *buffer_list;			/* virtual address of Rx & Tx buffer lists */
	unsigned long buffer_list_phys;

	unsigned int rx_buf_count;		/* count of total allocated Rx buffers */
	SCADESC *rx_buf_list;   		/* list of receive buffer entries */
	SCADESC_EX rx_buf_list_ex[SCAMAXDESC]; /* list of receive buffer entries */
	unsigned int current_rx_buf;

	unsigned int tx_buf_count;		/* count of total allocated Tx buffers */
	SCADESC *tx_buf_list;		/* list of transmit buffer entries */
	SCADESC_EX tx_buf_list_ex[SCAMAXDESC]; /* list of transmit buffer entries */
	unsigned int last_tx_buf;

	unsigned char *tmp_rx_buf;
	unsigned int tmp_rx_buf_count;

	int rx_enabled;
	int rx_overflow;

	int tx_enabled;
	int tx_active;
	u32 idle_mode;

	unsigned char ie0_value;
	unsigned char ie1_value;
	unsigned char ie2_value;
	unsigned char ctrlreg_value;
	unsigned char old_signals;

	char device_name[25];			/* device instance name */

	int port_count;
	int adapter_num;
	int port_num;

	struct _synclinkmp_info *port_array[SCA_MAX_PORTS];

	unsigned int bus_type;			/* expansion bus type (ISA,EISA,PCI) */

	unsigned int irq_level;			/* interrupt level */
	unsigned long irq_flags;
	int irq_requested;			/* nonzero if IRQ requested */

	MGSL_PARAMS params;			/* communications parameters */

	unsigned char serial_signals;		/* current serial signal states */

	int irq_occurred;			/* for diagnostics use */
	unsigned int init_error;		/* Initialization startup error */

	u32 last_mem_alloc;
	unsigned char* memory_base;		/* shared memory address (PCI only) */
	u32 phys_memory_base;
    	int shared_mem_requested;

	unsigned char* sca_base;		/* HD64570 SCA Memory address */
	u32 phys_sca_base;
	u32 sca_offset;
	int sca_base_requested;

	unsigned char* lcr_base;		/* local config registers (PCI only) */
	u32 phys_lcr_base;
	u32 lcr_offset;
	int lcr_mem_requested;

	unsigned char* statctrl_base;		/* status/control register memory */
	u32 phys_statctrl_base;
	u32 statctrl_offset;
	int sca_statctrl_requested;

	u32 misc_ctrl_value;
	char flag_buf[MAX_ASYNC_BUFFER_SIZE];
	char char_buf[MAX_ASYNC_BUFFER_SIZE];
	BOOLEAN drop_rts_on_tx_done;

	struct	_input_signal_events	input_signal_events;

	/* SPPP/Cisco HDLC device parts */
	int netcount;
	int dosyncppp;
	spinlock_t netlock;
#ifdef CONFIG_SYNCLINK_SYNCPPP
	struct ppp_device pppdev;
	char netname[10];
	struct net_device *netdev;
	struct net_device_stats netstats;
	struct net_device netdevice;
#endif
} SLMP_INFO;

#define MGSL_MAGIC 0x5401

/*
 * define serial signal status change macros
 */
#define	MISCSTATUS_DCD_LATCHED	(SerialSignal_DCD<<8)	/* indicates change in DCD */
#define MISCSTATUS_RI_LATCHED	(SerialSignal_RI<<8)	/* indicates change in RI */
#define MISCSTATUS_CTS_LATCHED	(SerialSignal_CTS<<8)	/* indicates change in CTS */
#define MISCSTATUS_DSR_LATCHED	(SerialSignal_DSR<<8)	/* change in DSR */

/* Common Register macros */
#define LPR	0x00
#define PABR0	0x02
#define PABR1	0x03
#define WCRL	0x04
#define WCRM	0x05
#define WCRH	0x06
#define DPCR	0x08
#define DMER	0x09
#define ISR0	0x10
#define ISR1	0x11
#define ISR2	0x12
#define IER0	0x14
#define IER1	0x15
#define IER2	0x16
#define ITCR	0x18
#define INTVR 	0x1a
#define IMVR	0x1c

/* MSCI Register macros */
#define TRB	0x20
#define TRBL	0x20
#define TRBH	0x21
#define SR0	0x22
#define SR1	0x23
#define SR2	0x24
#define SR3	0x25
#define FST	0x26
#define IE0	0x28
#define IE1	0x29
#define IE2	0x2a
#define FIE	0x2b
#define CMD	0x2c
#define MD0	0x2e
#define MD1	0x2f
#define MD2	0x30
#define CTL	0x31
#define SA0	0x32
#define SA1	0x33
#define IDL	0x34
#define TMC	0x35
#define RXS	0x36
#define TXS	0x37
#define TRC0	0x38
#define TRC1	0x39
#define RRC	0x3a
#define CST0	0x3c
#define CST1	0x3d

/* Timer Register Macros */
#define TCNT	0x60
#define TCNTL	0x60
#define TCNTH	0x61
#define TCONR	0x62
#define TCONRL	0x62
#define TCONRH	0x63
#define TMCS	0x64
#define TEPR	0x65

/* DMA Controller Register macros */
#define DAR	0x80
#define DARL	0x80
#define DARH	0x81
#define DARB	0x82
#define BAR	0x80
#define BARL	0x80
#define BARH	0x81
#define BARB	0x82
#define SAR	0x84
#define SARL	0x84
#define SARH	0x85
#define SARB	0x86
#define CPB	0x86
#define CDA	0x88
#define CDAL	0x88
#define CDAH	0x89
#define EDA	0x8a
#define EDAL	0x8a
#define EDAH	0x8b
#define BFL	0x8c
#define BFLL	0x8c
#define BFLH	0x8d
#define BCR	0x8e
#define BCRL	0x8e
#define BCRH	0x8f
#define DSR	0x90
#define DMR	0x91
#define FCT	0x93
#define DIR	0x94
#define DCMD	0x95

/* combine with timer or DMA register address */
#define TIMER0	0x00
#define TIMER1	0x08
#define TIMER2	0x10
#define TIMER3	0x18
#define RXDMA 	0x00
#define TXDMA 	0x20

/* SCA Command Codes */
#define NOOP		0x00
#define TXRESET		0x01
#define TXENABLE	0x02
#define TXDISABLE	0x03
#define TXCRCINIT	0x04
#define TXCRCEXCL	0x05
#define TXEOM		0x06
#define TXABORT		0x07
#define MPON		0x08
#define TXBUFCLR	0x09
#define RXRESET		0x11
#define RXENABLE	0x12
#define RXDISABLE	0x13
#define RXCRCINIT	0x14
#define RXREJECT	0x15
#define SEARCHMP	0x16
#define RXCRCEXCL	0x17
#define RXCRCCALC	0x18
#define CHRESET		0x21
#define HUNT		0x31

/* DMA command codes */
#define SWABORT		0x01
#define FEICLEAR	0x02

/* IE0 */
#define TXINTE 		BIT7
#define RXINTE 		BIT6
#define TXRDYE 		BIT1
#define RXRDYE 		BIT0

/* IE1 & SR1 */
#define UDRN   	BIT7
#define IDLE   	BIT6
#define SYNCD  	BIT4
#define FLGD   	BIT4
#define CCTS   	BIT3
#define CDCD   	BIT2
#define BRKD   	BIT1
#define ABTD   	BIT1
#define GAPD   	BIT1
#define BRKE   	BIT0
#define IDLD	BIT0

/* IE2 & SR2 */
#define EOM	BIT7
#define PMP	BIT6
#define SHRT	BIT6
#define PE	BIT5
#define ABT	BIT5
#define FRME	BIT4
#define RBIT	BIT4
#define OVRN	BIT3
#define CRCE	BIT2


#define jiffies_from_ms(a) ((((a) * HZ)/1000)+1)

/*
 * Global linked list of SyncLink devices
 */
static SLMP_INFO *synclinkmp_device_list = NULL;
static int synclinkmp_adapter_count = -1;
static int synclinkmp_device_count = 0;

/*
 * Set this param to non-zero to load eax with the
 * .text section address and breakpoint on module load.
 * This is useful for use with gdb and add-symbol-file command.
 */
static int break_on_load=0;

/*
 * Driver major number, defaults to zero to get auto
 * assigned major number. May be forced as module parameter.
 */
static int ttymajor=0;
static int cuamajor=0;

/*
 * Array of user specified options for ISA adapters.
 */
static int debug_level = 0;
static int maxframe[MAX_DEVICES] = {0,};
static int dosyncppp[MAX_DEVICES] = {0,};

MODULE_PARM(break_on_load,"i");
MODULE_PARM(ttymajor,"i");
MODULE_PARM(cuamajor,"i");
MODULE_PARM(debug_level,"i");
MODULE_PARM(maxframe,"1-" __MODULE_STRING(MAX_DEVICES) "i");
MODULE_PARM(dosyncppp,"1-" __MODULE_STRING(MAX_DEVICES) "i");

static char *driver_name = "SyncLink MultiPort driver";
static char *driver_version = "$Revision: 1.1.1.1 $";

static int __devinit synclinkmp_init_one(struct pci_dev *dev,const struct pci_device_id *ent);
static void __devexit synclinkmp_remove_one(struct pci_dev *dev);

static struct pci_device_id synclinkmp_pci_tbl[] __devinitdata = {
	{ PCI_VENDOR_ID_MICROGATE, PCI_DEVICE_ID_MICROGATE_SCA, PCI_ANY_ID, PCI_ANY_ID, },
	{ 0, }, /* terminate list */
};
MODULE_DEVICE_TABLE(pci, synclinkmp_pci_tbl);

static struct pci_driver synclinkmp_pci_driver = {
	name:		"synclinkmp",
	id_table:	synclinkmp_pci_tbl,
	probe:		synclinkmp_init_one,
	remove:		__devexit_p(synclinkmp_remove_one),
};


static struct tty_driver serial_driver, callout_driver;
static int serial_refcount;

/* number of characters left in xmit buffer before we ask for more */
#define WAKEUP_CHARS 256

static struct tty_struct *serial_table[MAX_DEVICES];
static struct termios *serial_termios[MAX_DEVICES];
static struct termios *serial_termios_locked[MAX_DEVICES];

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif


/* tty callbacks */

static int  open(struct tty_struct *tty, struct file * filp);
static void close(struct tty_struct *tty, struct file * filp);
static void hangup(struct tty_struct *tty);
static void set_termios(struct tty_struct *tty, struct termios *old_termios);

static int  write(struct tty_struct *tty, int from_user, const unsigned char *buf, int count);
static void put_char(struct tty_struct *tty, unsigned char ch);
static void send_xchar(struct tty_struct *tty, char ch);
static void wait_until_sent(struct tty_struct *tty, int timeout);
static int  write_room(struct tty_struct *tty);
static void flush_chars(struct tty_struct *tty);
static void flush_buffer(struct tty_struct *tty);
static void tx_hold(struct tty_struct *tty);
static void tx_release(struct tty_struct *tty);

static int  ioctl(struct tty_struct *tty, struct file *file, unsigned int cmd, unsigned long arg);
static int  read_proc(char *page, char **start, off_t off, int count,int *eof, void *data);
static int  chars_in_buffer(struct tty_struct *tty);
static void throttle(struct tty_struct * tty);
static void unthrottle(struct tty_struct * tty);
static void set_break(struct tty_struct *tty, int break_state);

/* sppp support and callbacks */

#ifdef CONFIG_SYNCLINK_SYNCPPP
static void sppp_init(SLMP_INFO *info);
static void sppp_delete(SLMP_INFO *info);
static void sppp_rx_done(SLMP_INFO *info, char *buf, int size);
static void sppp_tx_done(SLMP_INFO *info);

static int  sppp_cb_open(struct net_device *d);
static int  sppp_cb_close(struct net_device *d);
static int  sppp_cb_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd);
static int  sppp_cb_tx(struct sk_buff *skb, struct net_device *dev);
static void sppp_cb_tx_timeout(struct net_device *dev);
static struct net_device_stats *sppp_cb_net_stats(struct net_device *dev);
#endif

/* ioctl handlers */

static int  get_stats(SLMP_INFO *info, struct mgsl_icount *user_icount);
static int  get_params(SLMP_INFO *info, MGSL_PARAMS *params);
static int  set_params(SLMP_INFO *info, MGSL_PARAMS *params);
static int  get_txidle(SLMP_INFO *info, int*idle_mode);
static int  set_txidle(SLMP_INFO *info, int idle_mode);
static int  tx_enable(SLMP_INFO *info, int enable);
static int  tx_abort(SLMP_INFO *info);
static int  rx_enable(SLMP_INFO *info, int enable);
static int  map_status(int signals);
static int  modem_input_wait(SLMP_INFO *info,int arg);
static int  wait_mgsl_event(SLMP_INFO *info, int *mask_ptr);
static int  get_modem_info(SLMP_INFO *info, unsigned int *value);
static int  set_modem_info(SLMP_INFO *info, unsigned int cmd,unsigned int *value);
static void set_break(struct tty_struct *tty, int break_state);

static void add_device(SLMP_INFO *info);
static void device_init(int adapter_num, struct pci_dev *pdev);
static int  claim_resources(SLMP_INFO *info);
static void release_resources(SLMP_INFO *info);

static int  startup(SLMP_INFO *info);
static int  block_til_ready(struct tty_struct *tty, struct file * filp,SLMP_INFO *info);
static void shutdown(SLMP_INFO *info);
static void program_hw(SLMP_INFO *info);
static void change_params(SLMP_INFO *info);

static int  init_adapter(SLMP_INFO *info);
static int  register_test(SLMP_INFO *info);
static int  irq_test(SLMP_INFO *info);
static int  loopback_test(SLMP_INFO *info);
static int  adapter_test(SLMP_INFO *info);
static int  memory_test(SLMP_INFO *info);

static void reset_adapter(SLMP_INFO *info);
static void reset_port(SLMP_INFO *info);
static void async_mode(SLMP_INFO *info);
static void hdlc_mode(SLMP_INFO *info);

static void rx_stop(SLMP_INFO *info);
static void rx_start(SLMP_INFO *info);
static void rx_reset_buffers(SLMP_INFO *info);
static void rx_free_frame_buffers(SLMP_INFO *info, unsigned int first, unsigned int last);
static int  rx_get_frame(SLMP_INFO *info);

static void tx_start(SLMP_INFO *info);
static void tx_stop(SLMP_INFO *info);
static void tx_load_fifo(SLMP_INFO *info);
static void tx_set_idle(SLMP_INFO *info);
static void tx_load_dma_buffer(SLMP_INFO *info, const char *buf, unsigned int count);

static void get_signals(SLMP_INFO *info);
static void set_signals(SLMP_INFO *info);
static void enable_loopback(SLMP_INFO *info, int enable);
static void set_rate(SLMP_INFO *info, u32 data_rate);

static int  bh_action(SLMP_INFO *info);
static void bh_handler(void* Context);
static void bh_receive(SLMP_INFO *info);
static void bh_transmit(SLMP_INFO *info);
static void bh_status(SLMP_INFO *info);
static void isr_timer(SLMP_INFO *info);
static void isr_rxint(SLMP_INFO *info);
static void isr_rxrdy(SLMP_INFO *info);
static void isr_txint(SLMP_INFO *info);
static void isr_txrdy(SLMP_INFO *info);
static void isr_rxdmaok(SLMP_INFO *info);
static void isr_rxdmaerror(SLMP_INFO *info);
static void isr_txdmaok(SLMP_INFO *info);
static void isr_txdmaerror(SLMP_INFO *info);
static void isr_io_pin(SLMP_INFO *info, u16 status);
static void synclinkmp_interrupt(int irq, void *dev_id, struct pt_regs * regs);

static int  alloc_dma_bufs(SLMP_INFO *info);
static void free_dma_bufs(SLMP_INFO *info);
static int  alloc_buf_list(SLMP_INFO *info);
static int  alloc_frame_bufs(SLMP_INFO *info, SCADESC *list, SCADESC_EX *list_ex,int count);
static int  alloc_tmp_rx_buf(SLMP_INFO *info);
static void free_tmp_rx_buf(SLMP_INFO *info);

static void load_pci_memory(SLMP_INFO *info, char* dest, const char* src, unsigned short count);
static void trace_block(SLMP_INFO *info, const char* data, int count, int xmit);
static void tx_timeout(unsigned long context);
static void status_timeout(unsigned long context);

static unsigned char read_reg(SLMP_INFO *info, unsigned char addr);
static void write_reg(SLMP_INFO *info, unsigned char addr, unsigned char val);
static u16 read_reg16(SLMP_INFO *info, unsigned char addr);
static void write_reg16(SLMP_INFO *info, unsigned char addr, u16 val);
static unsigned char read_status_reg(SLMP_INFO * info);
static void write_control_reg(SLMP_INFO * info);


static unsigned char rx_active_fifo_level = 16;	// rx request FIFO activation level in bytes
static unsigned char tx_active_fifo_level = 16;	// tx request FIFO activation level in bytes
static unsigned char tx_negate_fifo_level = 32;	// tx request FIFO negation level in bytes

static u32 misc_ctrl_value = 0x007e4040;
static u32 lcr1_brdr_value = 0x0080002d;

static u32 read_ahead_count = 8;

/* DPCR, DMA Priority Control
 *
 * 07..05  Not used, must be 0
 * 04      BRC, bus release condition: 0=all transfers complete
 *              1=release after 1 xfer on all channels
 * 03      CCC, channel change condition: 0=every cycle
 *              1=after each channel completes all xfers
 * 02..00  PR<2..0>, priority 100=round robin
 *
 * 00000100 = 0x00
 */
static unsigned char dma_priority = 0x04;

// Number of bytes that can be written to shared RAM
// in a single write operation
static u32 sca_pci_load_interval = 64;

/*
 * 1st function defined in .text section. Calling this function in
 * init_module() followed by a breakpoint allows a remote debugger
 * (gdb) to get the .text address for the add-symbol-file command.
 * This allows remote debugging of dynamically loadable modules.
 */
static void* synclinkmp_get_text_ptr(void);
static void* synclinkmp_get_text_ptr() {return synclinkmp_get_text_ptr;}

static inline int sanity_check(SLMP_INFO *info,
			       kdev_t device, const char *routine)
{
#ifdef SANITY_CHECK
	static const char *badmagic =
		"Warning: bad magic number for synclinkmp_struct (%s) in %s\n";
	static const char *badinfo =
		"Warning: null synclinkmp_struct for (%s) in %s\n";

	if (!info) {
		printk(badinfo, kdevname(device), routine);
		return 1;
	}
	if (info->magic != MGSL_MAGIC) {
		printk(badmagic, kdevname(device), routine);
		return 1;
	}
#endif
	return 0;
}

/* tty callbacks */

/* Called when a port is opened.  Init and enable port.
 */
static int open(struct tty_struct *tty, struct file *filp)
{
	SLMP_INFO *info;
	int retval, line;
	unsigned long flags;

	line = MINOR(tty->device) - tty->driver.minor_start;
	if ((line < 0) || (line >= synclinkmp_device_count)) {
		printk("%s(%d): open with illegal line #%d.\n",
			__FILE__,__LINE__,line);
		return -ENODEV;
	}

	info = synclinkmp_device_list;
	while(info && info->line != line)
		info = info->next_device;
	if ( !info ){
		printk("%s(%d):%s Can't find specified device on open (line=%d)\n",
			__FILE__,__LINE__,info->device_name,line);
		return -ENODEV;
	}

	if ( info->init_error ) {
		printk("%s(%d):%s device is not allocated, init error=%d\n",
			__FILE__,__LINE__,info->device_name,info->init_error);
		return -ENODEV;
	}

	tty->driver_data = info;
	info->tty = tty;
	if (sanity_check(info, tty->device, "open"))
		return -ENODEV;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s open(), old ref count = %d\n",
			 __FILE__,__LINE__,tty->driver.name, info->count);

	MOD_INC_USE_COUNT;

	/* If port is closing, signal caller to try again */
	if (tty_hung_up_p(filp) || info->flags & ASYNC_CLOSING){
		if (info->flags & ASYNC_CLOSING)
			interruptible_sleep_on(&info->close_wait);
		retval = ((info->flags & ASYNC_HUP_NOTIFY) ?
			-EAGAIN : -ERESTARTSYS);
		goto cleanup;
	}

	info->tty->low_latency = (info->flags & ASYNC_LOW_LATENCY) ? 1 : 0;

	spin_lock_irqsave(&info->netlock, flags);
	if (info->netcount) {
		retval = -EBUSY;
		spin_unlock_irqrestore(&info->netlock, flags);
		goto cleanup;
	}
	info->count++;
	spin_unlock_irqrestore(&info->netlock, flags);

	if (info->count == 1) {
		/* 1st open on this device, init hardware */
		retval = startup(info);
		if (retval < 0)
			goto cleanup;
	}

	retval = block_til_ready(tty, filp, info);
	if (retval) {
		if (debug_level >= DEBUG_LEVEL_INFO)
			printk("%s(%d):%s block_til_ready() returned %d\n",
				 __FILE__,__LINE__, info->device_name, retval);
		goto cleanup;
	}

	if ((info->count == 1) &&
	    info->flags & ASYNC_SPLIT_TERMIOS) {
		if (tty->driver.subtype == SERIAL_TYPE_NORMAL)
			*tty->termios = info->normal_termios;
		else
			*tty->termios = info->callout_termios;
		change_params(info);
	}

	info->session = current->session;
	info->pgrp    = current->pgrp;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s open() success\n",
			 __FILE__,__LINE__, info->device_name);
	retval = 0;

cleanup:
	if (retval) {
		if(MOD_IN_USE)
			MOD_DEC_USE_COUNT;
		if(info->count)
			info->count--;
	}

	return retval;
}

/* Called when port is closed. Wait for remaining data to be
 * sent. Disable port and free resources.
 */
static void close(struct tty_struct *tty, struct file *filp)
{
	SLMP_INFO * info = (SLMP_INFO *)tty->driver_data;

	if (!info || sanity_check(info, tty->device, "close"))
		return;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s close() entry, count=%d\n",
			 __FILE__,__LINE__, info->device_name, info->count);

	if (!info->count || tty_hung_up_p(filp))
		goto cleanup;

	if ((tty->count == 1) && (info->count != 1)) {
		/*
		 * tty->count is 1 and the tty structure will be freed.
		 * info->count should be one in this case.
		 * if it's not, correct it so that the port is shutdown.
		 */
		printk("%s(%d):%s close: bad refcount; tty->count is 1, "
		       "info->count is %d\n",
			 __FILE__,__LINE__, info->device_name, info->count);
		info->count = 1;
	}

	info->count--;

	/* if at least one open remaining, leave hardware active */
	if (info->count)
		goto cleanup;

	info->flags |= ASYNC_CLOSING;

	/* Save the termios structure, since this port may have
	 * separate termios for callout and dialin.
	 */
	if (info->flags & ASYNC_NORMAL_ACTIVE)
		info->normal_termios = *tty->termios;
	if (info->flags & ASYNC_CALLOUT_ACTIVE)
		info->callout_termios = *tty->termios;

	/* set tty->closing to notify line discipline to
	 * only process XON/XOFF characters. Only the N_TTY
	 * discipline appears to use this (ppp does not).
	 */
	tty->closing = 1;

	/* wait for transmit data to clear all layers */

	if (info->closing_wait != ASYNC_CLOSING_WAIT_NONE) {
		if (debug_level >= DEBUG_LEVEL_INFO)
			printk("%s(%d):%s close() calling tty_wait_until_sent\n",
				 __FILE__,__LINE__, info->device_name );
		tty_wait_until_sent(tty, info->closing_wait);
	}

 	if (info->flags & ASYNC_INITIALIZED)
 		wait_until_sent(tty, info->timeout);

	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);

	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);

	shutdown(info);

	tty->closing = 0;
	info->tty = 0;

	if (info->blocked_open) {
		if (info->close_delay) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(info->close_delay);
		}
		wake_up_interruptible(&info->open_wait);
	}

	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE|
			 ASYNC_CLOSING);

	wake_up_interruptible(&info->close_wait);

cleanup:
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s close() exit, count=%d\n", __FILE__,__LINE__,
			tty->driver.name, info->count);
	if(MOD_IN_USE)
		MOD_DEC_USE_COUNT;
}

/* Called by tty_hangup() when a hangup is signaled.
 * This is the same as closing all open descriptors for the port.
 */
static void hangup(struct tty_struct *tty)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s hangup()\n",
			 __FILE__,__LINE__, info->device_name );

	if (sanity_check(info, tty->device, "hangup"))
		return;

	flush_buffer(tty);
	shutdown(info);

	info->count = 0;
	info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE);
	info->tty = 0;

	wake_up_interruptible(&info->open_wait);
}

/* Set new termios settings
 */
static void set_termios(struct tty_struct *tty, struct termios *old_termios)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s set_termios()\n", __FILE__,__LINE__,
			tty->driver.name );

	/* just return if nothing has changed */
	if ((tty->termios->c_cflag == old_termios->c_cflag)
	    && (RELEVANT_IFLAG(tty->termios->c_iflag)
		== RELEVANT_IFLAG(old_termios->c_iflag)))
	  return;

	change_params(info);

	/* Handle transition to B0 status */
	if (old_termios->c_cflag & CBAUD &&
	    !(tty->termios->c_cflag & CBAUD)) {
		info->serial_signals &= ~(SerialSignal_RTS + SerialSignal_DTR);
		spin_lock_irqsave(&info->lock,flags);
	 	set_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}

	/* Handle transition away from B0 status */
	if (!(old_termios->c_cflag & CBAUD) &&
	    tty->termios->c_cflag & CBAUD) {
		info->serial_signals |= SerialSignal_DTR;
 		if (!(tty->termios->c_cflag & CRTSCTS) ||
 		    !test_bit(TTY_THROTTLED, &tty->flags)) {
			info->serial_signals |= SerialSignal_RTS;
 		}
		spin_lock_irqsave(&info->lock,flags);
	 	set_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}

	/* Handle turning off CRTSCTS */
	if (old_termios->c_cflag & CRTSCTS &&
	    !(tty->termios->c_cflag & CRTSCTS)) {
		tty->hw_stopped = 0;
		tx_release(tty);
	}
}

/* Send a block of data
 *
 * Arguments:
 *
 * 	tty		pointer to tty information structure
 * 	from_user	flag: 1 = from user process
 * 	buf		pointer to buffer containing send data
 * 	count		size of send data in bytes
 *
 * Return Value:	number of characters written
 */
static int write(struct tty_struct *tty, int from_user,
		 const unsigned char *buf, int count)
{
	int	c, ret = 0, err;
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s write() count=%d\n",
		       __FILE__,__LINE__,info->device_name,count);

	if (sanity_check(info, tty->device, "write"))
		goto cleanup;

	if (!tty || !info->tx_buf)
		goto cleanup;

	if (info->params.mode == MGSL_MODE_HDLC) {
		if (count > info->max_frame_size) {
			ret = -EIO;
			goto cleanup;
		}
		if (info->tx_active)
			goto cleanup;
		if (info->tx_count) {
			/* send accumulated data from send_char() calls */
			/* as frame and wait before accepting more data. */
			tx_load_dma_buffer(info, info->tx_buf, info->tx_count);
			goto start;
		}
		if (!from_user) {
			ret = info->tx_count = count;
			tx_load_dma_buffer(info, buf, count);
			goto start;
		}
	}

	for (;;) {
		c = MIN(count,
			MIN(info->max_frame_size - info->tx_count - 1,
			    info->max_frame_size - info->tx_put));
		if (c <= 0)
			break;
			
		if (from_user) {
			COPY_FROM_USER(err, info->tx_buf + info->tx_put, buf, c);
			if (err) {
				if (!ret)
					ret = -EFAULT;
				break;
			}
		} else
			memcpy(info->tx_buf + info->tx_put, buf, c);

		spin_lock_irqsave(&info->lock,flags);
		info->tx_put += c;
		if (info->tx_put >= info->max_frame_size)
			info->tx_put -= info->max_frame_size;
		info->tx_count += c;
		spin_unlock_irqrestore(&info->lock,flags);

		buf += c;
		count -= c;
		ret += c;
	}

	if (info->params.mode == MGSL_MODE_HDLC) {
		if (count) {
			ret = info->tx_count = 0;
			goto cleanup;
		}
		tx_load_dma_buffer(info, info->tx_buf, info->tx_count);
	}
start:
 	if (info->tx_count && !tty->stopped && !tty->hw_stopped) {
		spin_lock_irqsave(&info->lock,flags);
		if (!info->tx_active)
		 	tx_start(info);
		spin_unlock_irqrestore(&info->lock,flags);
 	}

cleanup:
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk( "%s(%d):%s write() returning=%d\n",
			__FILE__,__LINE__,info->device_name,ret);
	return ret;
}

/* Add a character to the transmit buffer.
 */
static void put_char(struct tty_struct *tty, unsigned char ch)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if ( debug_level >= DEBUG_LEVEL_INFO ) {
		printk( "%s(%d):%s put_char(%d)\n",
			__FILE__,__LINE__,info->device_name,ch);
	}

	if (sanity_check(info, tty->device, "put_char"))
		return;

	if (!tty || !info->tx_buf)
		return;

	spin_lock_irqsave(&info->lock,flags);

	if ( (info->params.mode != MGSL_MODE_HDLC) ||
	     !info->tx_active ) {

		if (info->tx_count < info->max_frame_size - 1) {
			info->tx_buf[info->tx_put++] = ch;
			if (info->tx_put >= info->max_frame_size)
				info->tx_put -= info->max_frame_size;
			info->tx_count++;
		}
	}

	spin_unlock_irqrestore(&info->lock,flags);
}

/* Send a high-priority XON/XOFF character
 */
static void send_xchar(struct tty_struct *tty, char ch)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s send_xchar(%d)\n",
			 __FILE__,__LINE__, info->device_name, ch );

	if (sanity_check(info, tty->device, "send_xchar"))
		return;

	info->x_char = ch;
	if (ch) {
		/* Make sure transmit interrupts are on */
		spin_lock_irqsave(&info->lock,flags);
		if (!info->tx_enabled)
		 	tx_start(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}
}

/* Wait until the transmitter is empty.
 */
static void wait_until_sent(struct tty_struct *tty, int timeout)
{
	SLMP_INFO * info = (SLMP_INFO *)tty->driver_data;
	unsigned long orig_jiffies, char_time;

	if (!info )
		return;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s wait_until_sent() entry\n",
			 __FILE__,__LINE__, info->device_name );

	if (sanity_check(info, tty->device, "wait_until_sent"))
		return;

	if (!(info->flags & ASYNC_INITIALIZED))
		goto exit;

	orig_jiffies = jiffies;

	/* Set check interval to 1/5 of estimated time to
	 * send a character, and make it at least 1. The check
	 * interval should also be less than the timeout.
	 * Note: use tight timings here to satisfy the NIST-PCTS.
	 */

	if ( info->params.data_rate ) {
	       	char_time = info->timeout/(32 * 5);
		if (!char_time)
			char_time++;
	} else
		char_time = 1;

	if (timeout)
		char_time = MIN(char_time, timeout);

	if ( info->params.mode == MGSL_MODE_HDLC ) {
		while (info->tx_active) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(char_time);
			if (signal_pending(current))
				break;
			if (timeout && ((orig_jiffies + timeout) < jiffies))
				break;
		}
	} else {
		//TODO: determine if there is something similar to USC16C32
		// 	TXSTATUS_ALL_SENT status
		while ( info->tx_active && info->tx_enabled) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(char_time);
			if (signal_pending(current))
				break;
			if (timeout && ((orig_jiffies + timeout) < jiffies))
				break;
		}
	}

exit:
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s wait_until_sent() exit\n",
			 __FILE__,__LINE__, info->device_name );
}

/* Return the count of free bytes in transmit buffer
 */
static int write_room(struct tty_struct *tty)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	int ret;

	if (sanity_check(info, tty->device, "write_room"))
		return 0;

	if (info->params.mode == MGSL_MODE_HDLC) {
		ret = (info->tx_active) ? 0 : HDLC_MAX_FRAME_SIZE;
	} else {
		ret = info->max_frame_size - info->tx_count - 1;
		if (ret < 0)
			ret = 0;
	}

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s write_room()=%d\n",
		       __FILE__, __LINE__, info->device_name, ret);

	return ret;
}

/* enable transmitter and send remaining buffered characters
 */
static void flush_chars(struct tty_struct *tty)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):%s flush_chars() entry tx_count=%d\n",
			__FILE__,__LINE__,info->device_name,info->tx_count);

	if (sanity_check(info, tty->device, "flush_chars"))
		return;

	if (info->tx_count <= 0 || tty->stopped || tty->hw_stopped ||
	    !info->tx_buf)
		return;

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):%s flush_chars() entry, starting transmitter\n",
			__FILE__,__LINE__,info->device_name );

	spin_lock_irqsave(&info->lock,flags);

	if (!info->tx_active) {
		if ( (info->params.mode == MGSL_MODE_HDLC) &&
			info->tx_count ) {
			/* operating in synchronous (frame oriented) mode */
			/* copy data from circular tx_buf to */
			/* transmit DMA buffer. */
			tx_load_dma_buffer(info,
				 info->tx_buf,info->tx_count);
		}
	 	tx_start(info);
	}

	spin_unlock_irqrestore(&info->lock,flags);
}

/* Discard all data in the send buffer
 */
static void flush_buffer(struct tty_struct *tty)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s flush_buffer() entry\n",
			 __FILE__,__LINE__, info->device_name );

	if (sanity_check(info, tty->device, "flush_buffer"))
		return;

	spin_lock_irqsave(&info->lock,flags);
	info->tx_count = info->tx_put = info->tx_get = 0;
	del_timer(&info->tx_timer);
	spin_unlock_irqrestore(&info->lock,flags);

	wake_up_interruptible(&tty->write_wait);
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
	    tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);
}

/* throttle (stop) transmitter
 */
static void tx_hold(struct tty_struct *tty)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->device, "tx_hold"))
		return;

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk("%s(%d):%s tx_hold()\n",
			__FILE__,__LINE__,info->device_name);

	spin_lock_irqsave(&info->lock,flags);
	if (info->tx_enabled)
	 	tx_stop(info);
	spin_unlock_irqrestore(&info->lock,flags);
}

/* release (start) transmitter
 */
static void tx_release(struct tty_struct *tty)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if (sanity_check(info, tty->device, "tx_release"))
		return;

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk("%s(%d):%s tx_release()\n",
			__FILE__,__LINE__,info->device_name);

	spin_lock_irqsave(&info->lock,flags);
	if (!info->tx_enabled)
	 	tx_start(info);
	spin_unlock_irqrestore(&info->lock,flags);
}

/* Service an IOCTL request
 *
 * Arguments:
 *
 * 	tty	pointer to tty instance data
 * 	file	pointer to associated file object for device
 * 	cmd	IOCTL command code
 * 	arg	command argument/context
 *
 * Return Value:	0 if success, otherwise error code
 */
static int ioctl(struct tty_struct *tty, struct file *file,
		 unsigned int cmd, unsigned long arg)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	int error;
	struct mgsl_icount cnow;	/* kernel counter temps */
	struct serial_icounter_struct *p_cuser;	/* user space */
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s ioctl() cmd=%08X\n", __FILE__,__LINE__,
			info->device_name, cmd );

	if (sanity_check(info, tty->device, "ioctl"))
		return -ENODEV;

	if ((cmd != TIOCGSERIAL) && (cmd != TIOCSSERIAL) &&
	    (cmd != TIOCMIWAIT) && (cmd != TIOCGICOUNT)) {
		if (tty->flags & (1 << TTY_IO_ERROR))
		    return -EIO;
	}

	switch (cmd) {
	case TIOCMGET:
		return get_modem_info(info, (unsigned int *) arg);
	case TIOCMBIS:
	case TIOCMBIC:
	case TIOCMSET:
		return set_modem_info(info, cmd, (unsigned int *) arg);
	case MGSL_IOCGPARAMS:
		return get_params(info,(MGSL_PARAMS *)arg);
	case MGSL_IOCSPARAMS:
		return set_params(info,(MGSL_PARAMS *)arg);
	case MGSL_IOCGTXIDLE:
		return get_txidle(info,(int*)arg);
	case MGSL_IOCSTXIDLE:
		return set_txidle(info,(int)arg);
	case MGSL_IOCTXENABLE:
		return tx_enable(info,(int)arg);
	case MGSL_IOCRXENABLE:
		return rx_enable(info,(int)arg);
	case MGSL_IOCTXABORT:
		return tx_abort(info);
	case MGSL_IOCGSTATS:
		return get_stats(info,(struct mgsl_icount*)arg);
	case MGSL_IOCWAITEVENT:
		return wait_mgsl_event(info,(int*)arg);
	case MGSL_IOCLOOPTXDONE:
		return 0; // TODO: Not supported, need to document
	case MGSL_IOCCLRMODCOUNT:
		while(MOD_IN_USE)
			MOD_DEC_USE_COUNT;
		return 0;
		/* Wait for modem input (DCD,RI,DSR,CTS) change
		 * as specified by mask in arg (TIOCM_RNG/DSR/CD/CTS)
		 */
	case TIOCMIWAIT:
		return modem_input_wait(info,(int)arg);
		
		/*
		 * Get counter of input serial line interrupts (DCD,RI,DSR,CTS)
		 * Return: write counters to the user passed counter struct
		 * NB: both 1->0 and 0->1 transitions are counted except for
		 *     RI where only 0->1 is counted.
		 */
	case TIOCGICOUNT:
		spin_lock_irqsave(&info->lock,flags);
		cnow = info->icount;
		spin_unlock_irqrestore(&info->lock,flags);
		p_cuser = (struct serial_icounter_struct *) arg;
		PUT_USER(error,cnow.cts, &p_cuser->cts);
		if (error) return error;
		PUT_USER(error,cnow.dsr, &p_cuser->dsr);
		if (error) return error;
		PUT_USER(error,cnow.rng, &p_cuser->rng);
		if (error) return error;
		PUT_USER(error,cnow.dcd, &p_cuser->dcd);
		if (error) return error;
		PUT_USER(error,cnow.rx, &p_cuser->rx);
		if (error) return error;
		PUT_USER(error,cnow.tx, &p_cuser->tx);
		if (error) return error;
		PUT_USER(error,cnow.frame, &p_cuser->frame);
		if (error) return error;
		PUT_USER(error,cnow.overrun, &p_cuser->overrun);
		if (error) return error;
		PUT_USER(error,cnow.parity, &p_cuser->parity);
		if (error) return error;
		PUT_USER(error,cnow.brk, &p_cuser->brk);
		if (error) return error;
		PUT_USER(error,cnow.buf_overrun, &p_cuser->buf_overrun);
		if (error) return error;
		return 0;
	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

/*
 * /proc fs routines....
 */

static inline int line_info(char *buf, SLMP_INFO *info)
{
	char	stat_buf[30];
	int	ret;
	unsigned long flags;

	ret = sprintf(buf, "%s: SCABase=%08x Mem=%08X StatusControl=%08x LCR=%08X\n"
		       "\tIRQ=%d MaxFrameSize=%u\n",
		info->device_name,
		info->phys_sca_base,
		info->phys_memory_base,
		info->phys_statctrl_base,
		info->phys_lcr_base,
		info->irq_level,
		info->max_frame_size );

	/* output current serial signal states */
	spin_lock_irqsave(&info->lock,flags);
 	get_signals(info);
	spin_unlock_irqrestore(&info->lock,flags);

	stat_buf[0] = 0;
	stat_buf[1] = 0;
	if (info->serial_signals & SerialSignal_RTS)
		strcat(stat_buf, "|RTS");
	if (info->serial_signals & SerialSignal_CTS)
		strcat(stat_buf, "|CTS");
	if (info->serial_signals & SerialSignal_DTR)
		strcat(stat_buf, "|DTR");
	if (info->serial_signals & SerialSignal_DSR)
		strcat(stat_buf, "|DSR");
	if (info->serial_signals & SerialSignal_DCD)
		strcat(stat_buf, "|CD");
	if (info->serial_signals & SerialSignal_RI)
		strcat(stat_buf, "|RI");

	if (info->params.mode == MGSL_MODE_HDLC) {
		ret += sprintf(buf+ret, "\tHDLC txok:%d rxok:%d",
			      info->icount.txok, info->icount.rxok);
		if (info->icount.txunder)
			ret += sprintf(buf+ret, " txunder:%d", info->icount.txunder);
		if (info->icount.txabort)
			ret += sprintf(buf+ret, " txabort:%d", info->icount.txabort);
		if (info->icount.rxshort)
			ret += sprintf(buf+ret, " rxshort:%d", info->icount.rxshort);
		if (info->icount.rxlong)
			ret += sprintf(buf+ret, " rxlong:%d", info->icount.rxlong);
		if (info->icount.rxover)
			ret += sprintf(buf+ret, " rxover:%d", info->icount.rxover);
		if (info->icount.rxcrc)
			ret += sprintf(buf+ret, " rxlong:%d", info->icount.rxcrc);
	} else {
		ret += sprintf(buf+ret, "\tASYNC tx:%d rx:%d",
			      info->icount.tx, info->icount.rx);
		if (info->icount.frame)
			ret += sprintf(buf+ret, " fe:%d", info->icount.frame);
		if (info->icount.parity)
			ret += sprintf(buf+ret, " pe:%d", info->icount.parity);
		if (info->icount.brk)
			ret += sprintf(buf+ret, " brk:%d", info->icount.brk);
		if (info->icount.overrun)
			ret += sprintf(buf+ret, " oe:%d", info->icount.overrun);
	}

	/* Append serial signal status to end */
	ret += sprintf(buf+ret, " %s\n", stat_buf+1);

	ret += sprintf(buf+ret, "\ttxactive=%d bh_req=%d bh_run=%d pending_bh=%x\n",
	 info->tx_active,info->bh_requested,info->bh_running,
	 info->pending_bh);

	return ret;
}

/* Called to print information about devices
 */
int read_proc(char *page, char **start, off_t off, int count,
	      int *eof, void *data)
{
	int len = 0, l;
	off_t	begin = 0;
	SLMP_INFO *info;

	len += sprintf(page, "synclinkmp driver:%s\n", driver_version);

	info = synclinkmp_device_list;
	while( info ) {
		l = line_info(page + len, info);
		len += l;
		if (len+begin > off+count)
			goto done;
		if (len+begin < off) {
			begin += len;
			len = 0;
		}
		info = info->next_device;
	}

	*eof = 1;
done:
	if (off >= len+begin)
		return 0;
	*start = page + (off-begin);
	return ((count < begin+len-off) ? count : begin+len-off);
}

/* Return the count of bytes in transmit buffer
 */
static int chars_in_buffer(struct tty_struct *tty)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;

	if (sanity_check(info, tty->device, "chars_in_buffer"))
		return 0;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s chars_in_buffer()=%d\n",
		       __FILE__, __LINE__, info->device_name, info->tx_count);

	return info->tx_count;
}

/* Signal remote device to throttle send data (our receive data)
 */
static void throttle(struct tty_struct * tty)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s throttle() entry\n",
			 __FILE__,__LINE__, info->device_name );

	if (sanity_check(info, tty->device, "throttle"))
		return;

	if (I_IXOFF(tty))
		send_xchar(tty, STOP_CHAR(tty));

 	if (tty->termios->c_cflag & CRTSCTS) {
		spin_lock_irqsave(&info->lock,flags);
		info->serial_signals &= ~SerialSignal_RTS;
	 	set_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}
}

/* Signal remote device to stop throttling send data (our receive data)
 */
static void unthrottle(struct tty_struct * tty)
{
	SLMP_INFO *info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s unthrottle() entry\n",
			 __FILE__,__LINE__, info->device_name );

	if (sanity_check(info, tty->device, "unthrottle"))
		return;

	if (I_IXOFF(tty)) {
		if (info->x_char)
			info->x_char = 0;
		else
			send_xchar(tty, START_CHAR(tty));
	}

 	if (tty->termios->c_cflag & CRTSCTS) {
		spin_lock_irqsave(&info->lock,flags);
		info->serial_signals |= SerialSignal_RTS;
	 	set_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);
	}
}

/* set or clear transmit break condition
 * break_state	-1=set break condition, 0=clear
 */
static void set_break(struct tty_struct *tty, int break_state)
{
	unsigned char RegValue;
	SLMP_INFO * info = (SLMP_INFO *)tty->driver_data;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s set_break(%d)\n",
			 __FILE__,__LINE__, info->device_name, break_state);

	if (sanity_check(info, tty->device, "set_break"))
		return;

	spin_lock_irqsave(&info->lock,flags);
	RegValue = read_reg(info, CTL);
 	if (break_state == -1)
		RegValue |= BIT3;
	else
		RegValue &= ~BIT3;
	write_reg(info, CTL, RegValue);
	spin_unlock_irqrestore(&info->lock,flags);
}

#ifdef CONFIG_SYNCLINK_SYNCPPP

/* syncppp support and callbacks */

static void sppp_init(SLMP_INFO *info)
{
	struct net_device *d;

	sprintf(info->netname,"mgslm%dp%d",info->adapter_num,info->port_num);

	info->if_ptr = &info->pppdev;
	info->netdev = info->pppdev.dev = &info->netdevice;

	sppp_attach(&info->pppdev);

	d = info->netdev;
	strcpy(d->name,info->netname);
	d->base_addr = 0;
	d->irq = info->irq_level;
	d->dma = 0;
	d->priv = info;
	d->init = NULL;
	d->open = sppp_cb_open;
	d->stop = sppp_cb_close;
	d->hard_start_xmit = sppp_cb_tx;
	d->do_ioctl = sppp_cb_ioctl;
	d->get_stats = sppp_cb_net_stats;
	d->tx_timeout = sppp_cb_tx_timeout;
	d->watchdog_timeo = 10*HZ;

#if LINUX_VERSION_CODE < VERSION(2,4,4) 
	dev_init_buffers(d);
#endif

	if (register_netdev(d) == -1) {
		printk(KERN_WARNING "%s: register_netdev failed.\n", d->name);
		sppp_detach(info->netdev);
		return;
	}

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("sppp_init(%s)\n",info->netname);
}

static void sppp_delete(SLMP_INFO *info)
{
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("sppp_delete(%s)\n",info->netname);
	sppp_detach(info->netdev);
	unregister_netdev(info->netdev);
}

static int sppp_cb_open(struct net_device *d)
{
	SLMP_INFO *info = d->priv;
	int err, flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("sppp_cb_open(%s)\n",info->netname);

	spin_lock_irqsave(&info->netlock, flags);
	if (info->count != 0 || info->netcount != 0) {
		printk(KERN_WARNING "%s: sppp_cb_open returning busy\n", info->netname);
		spin_unlock_irqrestore(&info->netlock, flags);
		return -EBUSY;
	}
	info->netcount=1;
	MOD_INC_USE_COUNT;
	spin_unlock_irqrestore(&info->netlock, flags);

	/* claim resources and init adapter */
	if ((err = startup(info)) != 0)
		goto open_fail;

	/* allow syncppp module to do open processing */
	if ((err = sppp_open(d)) != 0) {
		shutdown(info);
		goto open_fail;
	}

	info->serial_signals |= SerialSignal_RTS + SerialSignal_DTR;
	program_hw(info);

	d->trans_start = jiffies;
	netif_start_queue(d);
	return 0;

open_fail:
	spin_lock_irqsave(&info->netlock, flags);
	info->netcount=0;
	MOD_DEC_USE_COUNT;
	spin_unlock_irqrestore(&info->netlock, flags);
	return err;
}

static void sppp_cb_tx_timeout(struct net_device *dev)
{
	SLMP_INFO *info = dev->priv;
	int flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("sppp_tx_timeout(%s)\n",info->netname);

	info->netstats.tx_errors++;
	info->netstats.tx_aborted_errors++;

	spin_lock_irqsave(&info->lock,flags);
	tx_stop(info);
	spin_unlock_irqrestore(&info->lock,flags);

	netif_wake_queue(dev);
}

static int sppp_cb_tx(struct sk_buff *skb, struct net_device *dev)
{
	SLMP_INFO *info = dev->priv;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("sppp_tx(%s)\n",info->netname);

	netif_stop_queue(dev);

	info->tx_count = skb->len;
	tx_load_dma_buffer(info, skb->data, skb->len);
	info->netstats.tx_packets++;
	info->netstats.tx_bytes += skb->len;
	dev_kfree_skb(skb);

	dev->trans_start = jiffies;

	spin_lock_irqsave(&info->lock,flags);
	if (!info->tx_active)
	 	tx_start(info);
	spin_unlock_irqrestore(&info->lock,flags);

	return 0;
}

static int sppp_cb_close(struct net_device *d)
{
	SLMP_INFO *info = d->priv;
	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("sppp_cb_close(%s)\n",info->netname);

	/* shutdown adapter and release resources */
	shutdown(info);

	/* allow syncppp to do close processing */
	sppp_close(d);
	netif_stop_queue(d);

	spin_lock_irqsave(&info->netlock, flags);
	info->netcount=0;
	MOD_DEC_USE_COUNT;
	spin_unlock_irqrestore(&info->netlock, flags);
	return 0;
}

static void sppp_rx_done(SLMP_INFO *info, char *buf, int size)
{
	struct sk_buff *skb = dev_alloc_skb(size);
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("sppp_rx_done(%s)\n",info->netname);
	if (skb == NULL) {
		printk(KERN_NOTICE "%s: cant alloc skb, dropping packet\n",
			info->netname);
		info->netstats.rx_dropped++;
		return;
	}

	memcpy(skb_put(skb, size),buf,size);

	skb->protocol = htons(ETH_P_WAN_PPP);
	skb->dev = info->netdev;
	skb->mac.raw = skb->data;
	info->netstats.rx_packets++;
	info->netstats.rx_bytes += size;
	netif_rx(skb);
	info->netdev->trans_start = jiffies;
}

static void sppp_tx_done(SLMP_INFO *info)
{
	if (netif_queue_stopped(info->netdev))
	    netif_wake_queue(info->netdev);
}

static struct net_device_stats *sppp_cb_net_stats(struct net_device *dev)
{
	SLMP_INFO *info = dev->priv;
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("net_stats(%s)\n",info->netname);
	return &info->netstats;
}

static int sppp_cb_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	SLMP_INFO *info = (SLMP_INFO *)dev->priv;
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):ioctl %s cmd=%08X\n", __FILE__,__LINE__,
			info->netname, cmd );
	return sppp_do_ioctl(dev, ifr, cmd);
}

#endif /* ifdef CONFIG_SYNCLINK_SYNCPPP */


/* Return next bottom half action to perform.
 * Return Value:	BH action code or 0 if nothing to do.
 */
int bh_action(SLMP_INFO *info)
{
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&info->lock,flags);

	if (info->pending_bh & BH_RECEIVE) {
		info->pending_bh &= ~BH_RECEIVE;
		rc = BH_RECEIVE;
	} else if (info->pending_bh & BH_TRANSMIT) {
		info->pending_bh &= ~BH_TRANSMIT;
		rc = BH_TRANSMIT;
	} else if (info->pending_bh & BH_STATUS) {
		info->pending_bh &= ~BH_STATUS;
		rc = BH_STATUS;
	}

	if (!rc) {
		/* Mark BH routine as complete */
		info->bh_running   = 0;
		info->bh_requested = 0;
	}

	spin_unlock_irqrestore(&info->lock,flags);

	return rc;
}

/* Perform bottom half processing of work items queued by ISR.
 */
void bh_handler(void* Context)
{
	SLMP_INFO *info = (SLMP_INFO*)Context;
	int action;

	if (!info)
		return;

	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):%s bh_handler() entry\n",
			__FILE__,__LINE__,info->device_name);

	info->bh_running = 1;

	while((action = bh_action(info)) != 0) {

		/* Process work item */
		if ( debug_level >= DEBUG_LEVEL_BH )
			printk( "%s(%d):%s bh_handler() work item action=%d\n",
				__FILE__,__LINE__,info->device_name, action);

		switch (action) {

		case BH_RECEIVE:
			bh_receive(info);
			break;
		case BH_TRANSMIT:
			bh_transmit(info);
			break;
		case BH_STATUS:
			bh_status(info);
			break;
		default:
			/* unknown work item ID */
			printk("%s(%d):%s Unknown work item ID=%08X!\n",
				__FILE__,__LINE__,info->device_name,action);
			break;
		}
	}

	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):%s bh_handler() exit\n",
			__FILE__,__LINE__,info->device_name);
}

void bh_receive(SLMP_INFO *info)
{
	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):%s bh_receive()\n",
			__FILE__,__LINE__,info->device_name);

	while( rx_get_frame(info) );
}

void bh_transmit(SLMP_INFO *info)
{
	struct tty_struct *tty = info->tty;

	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):%s bh_transmit() entry\n",
			__FILE__,__LINE__,info->device_name);

	if (tty) {
		if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
		    tty->ldisc.write_wakeup) {
			if ( debug_level >= DEBUG_LEVEL_BH )
				printk( "%s(%d):%s calling ldisc.write_wakeup\n",
					__FILE__,__LINE__,info->device_name);
			(tty->ldisc.write_wakeup)(tty);
		}
		wake_up_interruptible(&tty->write_wait);
	}
}

void bh_status(SLMP_INFO *info)
{
	if ( debug_level >= DEBUG_LEVEL_BH )
		printk( "%s(%d):%s bh_status() entry\n",
			__FILE__,__LINE__,info->device_name);

	info->ri_chkcount = 0;
	info->dsr_chkcount = 0;
	info->dcd_chkcount = 0;
	info->cts_chkcount = 0;
}

void isr_timer(SLMP_INFO * info)
{
	unsigned char timer = (info->port_num & 1) ? TIMER2 : TIMER0;

	/* IER2<7..4> = timer<3..0> interrupt enables (0=disabled) */
	write_reg(info, IER2, 0);

	/* TMCS, Timer Control/Status Register
	 *
	 * 07      CMF, Compare match flag (read only) 1=match
	 * 06      ECMI, CMF Interrupt Enable: 0=disabled
	 * 05      Reserved, must be 0
	 * 04      TME, Timer Enable
	 * 03..00  Reserved, must be 0
	 *
	 * 0000 0000
	 */
	write_reg(info, (unsigned char)(timer + TMCS), 0);

	info->irq_occurred = TRUE;

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_timer()\n",
			__FILE__,__LINE__,info->device_name);
}

void isr_rxint(SLMP_INFO * info)
{
 	struct tty_struct *tty = info->tty;
 	struct	mgsl_icount *icount = &info->icount;
	unsigned char status = read_reg(info, SR1);
	unsigned char status2 = read_reg(info, SR2);

	/* clear status bits */
	if ( status & (FLGD + IDLD + CDCD + BRKD) )
		write_reg(info, SR1, 
				(unsigned char)(status & (FLGD + IDLD + CDCD + BRKD)));

	if ( status2 & OVRN )
		write_reg(info, SR2, (unsigned char)(status2 & OVRN));
	
	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_rxint status=%02X %02x\n",
			__FILE__,__LINE__,info->device_name,status,status2);

	if (info->params.mode == MGSL_MODE_ASYNC) {
		if (status & BRKD) {
			icount->brk++;

			/* process break detection if tty control
			 * is not set to ignore it
			 */
			if ( tty ) {
				if (!(status & info->ignore_status_mask1)) {
					if (info->read_status_mask1 & BRKD) {
						*tty->flip.flag_buf_ptr = TTY_BREAK;
						if (info->flags & ASYNC_SAK)
							do_SAK(tty);
					}
				}
			}
		}
	}
	else {
		if (status & (FLGD|IDLD)) {
			if (status & FLGD)
				info->icount.exithunt++;
			else if (status & IDLD)
				info->icount.rxidle++;
			wake_up_interruptible(&info->event_wait_q);
		}
	}

	if (status & CDCD) {
		/* simulate a common modem status change interrupt
		 * for our handler
		 */
		get_signals( info );
		isr_io_pin(info,
			MISCSTATUS_DCD_LATCHED|(info->serial_signals&SerialSignal_DCD));
	}
}

/*
 * handle async rx data interrupts
 */
void isr_rxrdy(SLMP_INFO * info)
{
	u16 status;
	unsigned char DataByte;
 	struct tty_struct *tty = info->tty;
 	struct	mgsl_icount *icount = &info->icount;

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_rxrdy\n",
			__FILE__,__LINE__,info->device_name);

	while((status = read_reg(info,CST0)) & BIT0)
	{
		DataByte = read_reg(info,TRB);

		if ( tty ) {
			if (tty->flip.count >= TTY_FLIPBUF_SIZE)
				continue;

			*tty->flip.char_buf_ptr = DataByte;
			*tty->flip.flag_buf_ptr = 0;
		}

		icount->rx++;

		if ( status & (PE + FRME + OVRN) ) {
			printk("%s(%d):%s rxerr=%04X\n",
				__FILE__,__LINE__,info->device_name,status);

			/* update error statistics */
			if (status & PE)
				icount->parity++;
			else if (status & FRME)
				icount->frame++;
			else if (status & OVRN)
				icount->overrun++;

			/* discard char if tty control flags say so */
			if (status & info->ignore_status_mask2)
				continue;

			status &= info->read_status_mask2;

			if ( tty ) {
				if (status & PE)
					*tty->flip.flag_buf_ptr = TTY_PARITY;
				else if (status & FRME)
					*tty->flip.flag_buf_ptr = TTY_FRAME;
				if (status & OVRN) {
					/* Overrun is special, since it's
					 * reported immediately, and doesn't
					 * affect the current character
					 */
					if (tty->flip.count < TTY_FLIPBUF_SIZE) {
						tty->flip.count++;
						tty->flip.flag_buf_ptr++;
						tty->flip.char_buf_ptr++;
						*tty->flip.flag_buf_ptr = TTY_OVERRUN;
					}
				}
			}
		}	/* end of if (error) */

		if ( tty ) {
			tty->flip.flag_buf_ptr++;
			tty->flip.char_buf_ptr++;
			tty->flip.count++;
		}
	}

	if ( debug_level >= DEBUG_LEVEL_ISR ) {
		printk("%s(%d):%s isr_rxrdy() flip count=%d\n",
			__FILE__,__LINE__,info->device_name,
			tty ? tty->flip.count : 0);
		printk("%s(%d):%s rx=%d brk=%d parity=%d frame=%d overrun=%d\n",
			__FILE__,__LINE__,info->device_name,
			icount->rx,icount->brk,icount->parity,
			icount->frame,icount->overrun);
	}

	if ( tty && tty->flip.count )
		tty_flip_buffer_push(tty);
}

void isr_txeom(SLMP_INFO * info, unsigned char status)
{
	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_txeom status=%02x\n",
			__FILE__,__LINE__,info->device_name,status);

	/* disable and clear MSCI interrupts */
	info->ie1_value &= ~(IDLE + UDRN);
	write_reg(info, IE1, info->ie1_value);
	write_reg(info, SR1, (unsigned char)(UDRN + IDLE));

	write_reg(info, TXDMA + DIR, 0x00); /* disable Tx DMA IRQs */
	write_reg(info, TXDMA + DSR, 0xc0); /* clear IRQs and disable DMA */
	write_reg(info, TXDMA + DCMD, SWABORT);	/* reset/init DMA channel */

	if ( info->tx_active ) {
		if (info->params.mode != MGSL_MODE_ASYNC) {
			if (status & UDRN)
				info->icount.txunder++;
			else if (status & IDLE)
				info->icount.txok++;
		}

		info->tx_active = 0;
		info->tx_count = info->tx_put = info->tx_get = 0;

		del_timer(&info->tx_timer);

		if (info->params.mode != MGSL_MODE_ASYNC && info->drop_rts_on_tx_done ) {
			info->serial_signals &= ~SerialSignal_RTS;
			info->drop_rts_on_tx_done = 0;
			set_signals(info);
		}

#ifdef CONFIG_SYNCLINK_SYNCPPP
		if (info->netcount)
			sppp_tx_done(info);
		else
#endif
		{
			if (info->tty && (info->tty->stopped || info->tty->hw_stopped)) {
				tx_stop(info);
				return;
			}
			info->pending_bh |= BH_TRANSMIT;
		}
	}
}


/*
 * handle tx status interrupts
 */
void isr_txint(SLMP_INFO * info)
{
	unsigned char status = read_reg(info, SR1);

	/* clear status bits */
	write_reg(info, SR1, (unsigned char)(status & (UDRN + IDLE + CCTS)));

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_txint status=%02x\n",
			__FILE__,__LINE__,info->device_name,status);

	if (status & (UDRN + IDLE))
		isr_txeom(info, status);

	if (status & CCTS) {
		/* simulate a common modem status change interrupt
		 * for our handler
		 */
		get_signals( info );
		isr_io_pin(info,
			MISCSTATUS_CTS_LATCHED|(info->serial_signals&SerialSignal_CTS));

	}
}

/*
 * handle async tx data interrupts
 */
void isr_txrdy(SLMP_INFO * info)
{
	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_txrdy() tx_count=%d\n",
			__FILE__,__LINE__,info->device_name,info->tx_count);

	if (info->tty && (info->tty->stopped || info->tty->hw_stopped)) {
		tx_stop(info);
		return;
	}

	if ( info->tx_count )
		tx_load_fifo( info );
	else {
		info->tx_active = 0;
		info->ie0_value &= ~TXRDYE;
		write_reg(info, IE0, info->ie0_value);
	}

	if (info->tx_count < WAKEUP_CHARS)
		info->pending_bh |= BH_TRANSMIT;
}

void isr_rxdmaok(SLMP_INFO * info)
{
	/* BIT7 = EOT (end of transfer)
	 * BIT6 = EOM (end of message/frame)
	 */
	unsigned char status = read_reg(info,RXDMA + DSR) & 0xc0;

	/* clear IRQ (BIT0 must be 1 to prevent clearing DE bit) */
	write_reg(info, RXDMA + DSR, (unsigned char)(status | 1));

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_rxdmaok(), status=%02x\n",
			__FILE__,__LINE__,info->device_name,status);

	info->pending_bh |= BH_RECEIVE;
}

void isr_rxdmaerror(SLMP_INFO * info)
{
	/* BIT5 = BOF (buffer overflow)
	 * BIT4 = COF (counter overflow)
	 */
	unsigned char status = read_reg(info,RXDMA + DSR) & 0x30;

	/* clear IRQ (BIT0 must be 1 to prevent clearing DE bit) */
	write_reg(info, RXDMA + DSR, (unsigned char)(status | 1));

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_rxdmaerror(), status=%02x\n",
			__FILE__,__LINE__,info->device_name,status);

	info->rx_overflow = TRUE;
	info->pending_bh |= BH_RECEIVE;
}

void isr_txdmaok(SLMP_INFO * info)
{
	/* BIT7 = EOT (end of transfer, used for async mode)
	 * BIT6 = EOM (end of message/frame, used for sync mode)
	 *
	 * We don't look at DMA status because only EOT is enabled
	 * and we always clear and disable all tx DMA IRQs.
	 */
//	unsigned char dma_status = read_reg(info,TXDMA + DSR) & 0xc0;
	unsigned char status_reg1 = read_reg(info, SR1);

	write_reg(info, TXDMA + DIR, 0x00);	/* disable Tx DMA IRQs */
	write_reg(info, TXDMA + DSR, 0xc0); /* clear IRQs and disable DMA */
	write_reg(info, TXDMA + DCMD, SWABORT);	/* reset/init DMA channel */

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_txdmaok(), status=%02x\n",
			__FILE__,__LINE__,info->device_name,status_reg1);

	/* If transmitter already idle, do end of frame processing,
	 * otherwise enable interrupt for tx IDLE.
	 */
	if (status_reg1 & IDLE)
		isr_txeom(info, IDLE);
	else {
		/* disable and clear underrun IRQ, enable IDLE interrupt */
		info->ie1_value |= IDLE;
		info->ie1_value &= ~UDRN;
		write_reg(info, IE1, info->ie1_value);

		write_reg(info, SR1, UDRN);
	}
}

void isr_txdmaerror(SLMP_INFO * info)
{
	/* BIT5 = BOF (buffer overflow)
	 * BIT4 = COF (counter overflow)
	 */
	unsigned char status = read_reg(info,TXDMA + DSR) & 0x30;

	/* clear IRQ (BIT0 must be 1 to prevent clearing DE bit) */
	write_reg(info, TXDMA + DSR, (unsigned char)(status | 1));

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):%s isr_txdmaerror(), status=%02x\n",
			__FILE__,__LINE__,info->device_name,status);
}

/* handle input serial signal changes
 */
void isr_io_pin( SLMP_INFO *info, u16 status )
{
 	struct	mgsl_icount *icount;

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):isr_io_pin status=%04X\n",
			__FILE__,__LINE__,status);

	if (status & (MISCSTATUS_CTS_LATCHED | MISCSTATUS_DCD_LATCHED |
	              MISCSTATUS_DSR_LATCHED | MISCSTATUS_RI_LATCHED) ) {
		icount = &info->icount;
		/* update input line counters */
		if (status & MISCSTATUS_RI_LATCHED) {
			icount->rng++;
			if ( status & SerialSignal_RI )
				info->input_signal_events.ri_up++;
			else
				info->input_signal_events.ri_down++;
		}
		if (status & MISCSTATUS_DSR_LATCHED) {
			icount->dsr++;
			if ( status & SerialSignal_DSR )
				info->input_signal_events.dsr_up++;
			else
				info->input_signal_events.dsr_down++;
		}
		if (status & MISCSTATUS_DCD_LATCHED) {
			if ((info->dcd_chkcount)++ >= IO_PIN_SHUTDOWN_LIMIT) {
				info->ie1_value &= ~CDCD;
				write_reg(info, IE1, info->ie1_value);
			}
			icount->dcd++;
			if (status & SerialSignal_DCD) {
				info->input_signal_events.dcd_up++;
#ifdef CONFIG_SYNCLINK_SYNCPPP
				if (info->netcount)
					sppp_reopen(info->netdev);
#endif
			} else
				info->input_signal_events.dcd_down++;
		}
		if (status & MISCSTATUS_CTS_LATCHED)
		{
			if ((info->cts_chkcount)++ >= IO_PIN_SHUTDOWN_LIMIT) {
				info->ie1_value &= ~CCTS;
				write_reg(info, IE1, info->ie1_value);
			}
			icount->cts++;
			if ( status & SerialSignal_CTS )
				info->input_signal_events.cts_up++;
			else
				info->input_signal_events.cts_down++;
		}
		wake_up_interruptible(&info->status_event_wait_q);
		wake_up_interruptible(&info->event_wait_q);

		if ( (info->flags & ASYNC_CHECK_CD) &&
		     (status & MISCSTATUS_DCD_LATCHED) ) {
			if ( debug_level >= DEBUG_LEVEL_ISR )
				printk("%s CD now %s...", info->device_name,
				       (status & SerialSignal_DCD) ? "on" : "off");
			if (status & SerialSignal_DCD)
				wake_up_interruptible(&info->open_wait);
			else if (!((info->flags & ASYNC_CALLOUT_ACTIVE) &&
				   (info->flags & ASYNC_CALLOUT_NOHUP))) {
				if ( debug_level >= DEBUG_LEVEL_ISR )
					printk("doing serial hangup...");
				if (info->tty)
					tty_hangup(info->tty);
			}
		}

		if ( (info->flags & ASYNC_CTS_FLOW) &&
		     (status & MISCSTATUS_CTS_LATCHED) ) {
			if ( info->tty ) {
				if (info->tty->hw_stopped) {
					if (status & SerialSignal_CTS) {
						if ( debug_level >= DEBUG_LEVEL_ISR )
							printk("CTS tx start...");
			 			info->tty->hw_stopped = 0;
						tx_start(info);
						info->pending_bh |= BH_TRANSMIT;
						return;
					}
				} else {
					if (!(status & SerialSignal_CTS)) {
						if ( debug_level >= DEBUG_LEVEL_ISR )
							printk("CTS tx stop...");
			 			info->tty->hw_stopped = 1;
						tx_stop(info);
					}
				}
			}
		}
	}

	info->pending_bh |= BH_STATUS;
}

/* Interrupt service routine entry point.
 *
 * Arguments:
 * 	irq		interrupt number that caused interrupt
 * 	dev_id		device ID supplied during interrupt registration
 * 	regs		interrupted processor context
 */
static void synclinkmp_interrupt(int irq, void *dev_id, struct pt_regs * regs)
{
	SLMP_INFO * info;
	unsigned char status, status0, status1=0;
	unsigned char dmastatus, dmastatus0, dmastatus1=0;
	unsigned char timerstatus0, timerstatus1=0;
	unsigned char shift;
	unsigned int i;
	unsigned short tmp;

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d): synclinkmp_interrupt(%d)entry.\n",
			__FILE__,__LINE__,irq);

	info = (SLMP_INFO *)dev_id;
	if (!info)
		return;

	spin_lock(&info->lock);

	for(;;) {

		/* get status for SCA0 (ports 0-1) */
		tmp = read_reg16(info, ISR0);	/* get ISR0 and ISR1 in one read */
		status0 = (unsigned char)tmp;
		dmastatus0 = (unsigned char)(tmp>>8);
		timerstatus0 = read_reg(info, ISR2);

		if ( debug_level >= DEBUG_LEVEL_ISR )
			printk("%s(%d):%s status0=%02x, dmastatus0=%02x, timerstatus0=%02x\n",
				__FILE__,__LINE__,info->device_name,
				status0,dmastatus0,timerstatus0);

		if (info->port_count == 4) {
			/* get status for SCA1 (ports 2-3) */
			tmp = read_reg16(info->port_array[2], ISR0);
			status1 = (unsigned char)tmp;
			dmastatus1 = (unsigned char)(tmp>>8);
			timerstatus1 = read_reg(info->port_array[2], ISR2);

			if ( debug_level >= DEBUG_LEVEL_ISR )
				printk("%s(%d):%s status1=%02x, dmastatus1=%02x, timerstatus1=%02x\n",
					__FILE__,__LINE__,info->device_name,
					status1,dmastatus1,timerstatus1);
		}

		if (!status0 && !dmastatus0 && !timerstatus0 &&
			 !status1 && !dmastatus1 && !timerstatus1)
			break;

		for(i=0; i < info->port_count ; i++) {
			if (info->port_array[i] == NULL)
				continue;
			if (i < 2) {
				status = status0;
				dmastatus = dmastatus0;
			} else {
				status = status1;
				dmastatus = dmastatus1;
			}

			shift = i & 1 ? 4 :0;

			if (status & BIT0 << shift)
				isr_rxrdy(info->port_array[i]);
			if (status & BIT1 << shift)
				isr_txrdy(info->port_array[i]);
			if (status & BIT2 << shift)
				isr_rxint(info->port_array[i]);
			if (status & BIT3 << shift)
				isr_txint(info->port_array[i]);

			if (dmastatus & BIT0 << shift)
				isr_rxdmaerror(info->port_array[i]);
			if (dmastatus & BIT1 << shift)
				isr_rxdmaok(info->port_array[i]);
			if (dmastatus & BIT2 << shift)
				isr_txdmaerror(info->port_array[i]);
			if (dmastatus & BIT3 << shift)
				isr_txdmaok(info->port_array[i]);
		}

		if (timerstatus0 & (BIT5 | BIT4))
			isr_timer(info->port_array[0]);
		if (timerstatus0 & (BIT7 | BIT6))
			isr_timer(info->port_array[1]);
		if (timerstatus1 & (BIT5 | BIT4))
			isr_timer(info->port_array[2]);
		if (timerstatus1 & (BIT7 | BIT6))
			isr_timer(info->port_array[3]);
	}

	for(i=0; i < info->port_count ; i++) {
		SLMP_INFO * port = info->port_array[i];

		/* Request bottom half processing if there's something
		 * for it to do and the bh is not already running.
		 *
		 * Note: startup adapter diags require interrupts.
		 * do not request bottom half processing if the
		 * device is not open in a normal mode.
		 */
		if ( port && (port->count || port->netcount) &&
		     port->pending_bh && !port->bh_running &&
		     !port->bh_requested ) {
			if ( debug_level >= DEBUG_LEVEL_ISR )
				printk("%s(%d):%s queueing bh task.\n",
					__FILE__,__LINE__,port->device_name);
			queue_task(&port->task, &tq_immediate);
			mark_bh(IMMEDIATE_BH);
			port->bh_requested = 1;
		}
	}

	spin_unlock(&info->lock);

	if ( debug_level >= DEBUG_LEVEL_ISR )
		printk("%s(%d):synclinkmp_interrupt(%d)exit.\n",
			__FILE__,__LINE__,irq);
}

/* Initialize and start device.
 */
static int startup(SLMP_INFO * info)
{
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk("%s(%d):%s tx_releaseup()\n",__FILE__,__LINE__,info->device_name);

	if (info->flags & ASYNC_INITIALIZED)
		return 0;

	if (!info->tx_buf) {
		info->tx_buf = (unsigned char *)kmalloc(info->max_frame_size, GFP_KERNEL);
		if (!info->tx_buf) {
			printk(KERN_ERR"%s(%d):%s can't allocate transmit buffer\n",
				__FILE__,__LINE__,info->device_name);
			return -ENOMEM;
		}
	}

	info->pending_bh = 0;

	init_timer(&info->tx_timer);
	info->tx_timer.data = (unsigned long)info;
	info->tx_timer.function = tx_timeout;

	/* program hardware for current parameters */
	reset_port(info);

	change_params(info);

	init_timer(&info->status_timer);
	info->status_timer.data = (unsigned long)info;
	info->status_timer.function = status_timeout;
	info->status_timer.expires = jiffies + jiffies_from_ms(10);
	add_timer(&info->status_timer);

	if (info->tty)
		clear_bit(TTY_IO_ERROR, &info->tty->flags);

	info->flags |= ASYNC_INITIALIZED;

	return 0;
}

/* Called by close() and hangup() to shutdown hardware
 */
static void shutdown(SLMP_INFO * info)
{
	unsigned long flags;

	if (!(info->flags & ASYNC_INITIALIZED))
		return;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s synclinkmp_shutdown()\n",
			 __FILE__,__LINE__, info->device_name );

	/* clear status wait queue because status changes */
	/* can't happen after shutting down the hardware */
	wake_up_interruptible(&info->status_event_wait_q);
	wake_up_interruptible(&info->event_wait_q);

	del_timer(&info->tx_timer);
	del_timer(&info->status_timer);

	if (info->tx_buf) {
		free_page((unsigned long) info->tx_buf);
		info->tx_buf = 0;
	}

	spin_lock_irqsave(&info->lock,flags);

	reset_port(info);

 	if (!info->tty || info->tty->termios->c_cflag & HUPCL) {
 		info->serial_signals &= ~(SerialSignal_DTR + SerialSignal_RTS);
		set_signals(info);
	}

	spin_unlock_irqrestore(&info->lock,flags);

	if (info->tty)
		set_bit(TTY_IO_ERROR, &info->tty->flags);

	info->flags &= ~ASYNC_INITIALIZED;
}

static void program_hw(SLMP_INFO *info)
{
	unsigned long flags;

	spin_lock_irqsave(&info->lock,flags);

	rx_stop(info);
	tx_stop(info);

	info->tx_count = info->tx_put = info->tx_get = 0;

	if (info->params.mode == MGSL_MODE_HDLC || info->netcount)
		hdlc_mode(info);
	else
		async_mode(info);

	set_signals(info);

	info->dcd_chkcount = 0;
	info->cts_chkcount = 0;
	info->ri_chkcount = 0;
	info->dsr_chkcount = 0;

	info->ie1_value |= (CDCD|CCTS);
	write_reg(info, IE1, info->ie1_value);

	get_signals(info);

	if (info->netcount || (info->tty && info->tty->termios->c_cflag & CREAD) )
		rx_start(info);

	spin_unlock_irqrestore(&info->lock,flags);
}

/* Reconfigure adapter based on new parameters
 */
static void change_params(SLMP_INFO *info)
{
	unsigned cflag;
	int bits_per_char;

	if (!info->tty || !info->tty->termios)
		return;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s change_params()\n",
			 __FILE__,__LINE__, info->device_name );

	cflag = info->tty->termios->c_cflag;

	/* if B0 rate (hangup) specified then negate DTR and RTS */
	/* otherwise assert DTR and RTS */
 	if (cflag & CBAUD)
		info->serial_signals |= SerialSignal_RTS + SerialSignal_DTR;
	else
		info->serial_signals &= ~(SerialSignal_RTS + SerialSignal_DTR);

	/* byte size and parity */

	switch (cflag & CSIZE) {
	      case CS5: info->params.data_bits = 5; break;
	      case CS6: info->params.data_bits = 6; break;
	      case CS7: info->params.data_bits = 7; break;
	      case CS8: info->params.data_bits = 8; break;
	      /* Never happens, but GCC is too dumb to figure it out */
	      default:  info->params.data_bits = 7; break;
	      }

	if (cflag & CSTOPB)
		info->params.stop_bits = 2;
	else
		info->params.stop_bits = 1;

	info->params.parity = ASYNC_PARITY_NONE;
	if (cflag & PARENB) {
		if (cflag & PARODD)
			info->params.parity = ASYNC_PARITY_ODD;
		else
			info->params.parity = ASYNC_PARITY_EVEN;
#ifdef CMSPAR
		if (cflag & CMSPAR)
			info->params.parity = ASYNC_PARITY_SPACE;
#endif
	}

	/* calculate number of jiffies to transmit a full
	 * FIFO (32 bytes) at specified data rate
	 */
	bits_per_char = info->params.data_bits +
			info->params.stop_bits + 1;

	/* if port data rate is set to 460800 or less then
	 * allow tty settings to override, otherwise keep the
	 * current data rate.
	 */
	if (info->params.data_rate <= 460800) {
		info->params.data_rate = tty_get_baud_rate(info->tty);
	}

	if ( info->params.data_rate ) {
		info->timeout = (32*HZ*bits_per_char) /
				info->params.data_rate;
	}
	info->timeout += HZ/50;		/* Add .02 seconds of slop */

	if (cflag & CRTSCTS)
		info->flags |= ASYNC_CTS_FLOW;
	else
		info->flags &= ~ASYNC_CTS_FLOW;

	if (cflag & CLOCAL)
		info->flags &= ~ASYNC_CHECK_CD;
	else
		info->flags |= ASYNC_CHECK_CD;

	/* process tty input control flags */

	info->read_status_mask2 = OVRN;
	if (I_INPCK(info->tty))
		info->read_status_mask2 |= PE | FRME;
 	if (I_BRKINT(info->tty) || I_PARMRK(info->tty))
 		info->read_status_mask1 |= BRKD;
	if (I_IGNPAR(info->tty))
		info->ignore_status_mask2 |= PE | FRME;
	if (I_IGNBRK(info->tty)) {
		info->ignore_status_mask1 |= BRKD;
		/* If ignoring parity and break indicators, ignore
		 * overruns too.  (For real raw support).
		 */
		if (I_IGNPAR(info->tty))
			info->ignore_status_mask2 |= OVRN;
	}

	program_hw(info);
}

static int get_stats(SLMP_INFO * info, struct mgsl_icount *user_icount)
{
	int err;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s get_params()\n",
			 __FILE__,__LINE__, info->device_name);

	COPY_TO_USER(err,user_icount, &info->icount, sizeof(struct mgsl_icount));
	if (err) {
		if ( debug_level >= DEBUG_LEVEL_INFO )
			printk( "%s(%d):%s get_stats() user buffer copy failed\n",
				__FILE__,__LINE__,info->device_name);
		return -EFAULT;
	}

	return 0;
}

static int get_params(SLMP_INFO * info, MGSL_PARAMS *user_params)
{
	int err;
	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s get_params()\n",
			 __FILE__,__LINE__, info->device_name);

	COPY_TO_USER(err,user_params, &info->params, sizeof(MGSL_PARAMS));
	if (err) {
		if ( debug_level >= DEBUG_LEVEL_INFO )
			printk( "%s(%d):%s get_params() user buffer copy failed\n",
				__FILE__,__LINE__,info->device_name);
		return -EFAULT;
	}

	return 0;
}

static int set_params(SLMP_INFO * info, MGSL_PARAMS *new_params)
{
 	unsigned long flags;
	MGSL_PARAMS tmp_params;
	int err;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s set_params\n",
			__FILE__,__LINE__,info->device_name );
	COPY_FROM_USER(err,&tmp_params, new_params, sizeof(MGSL_PARAMS));
	if (err) {
		if ( debug_level >= DEBUG_LEVEL_INFO )
			printk( "%s(%d):%s set_params() user buffer copy failed\n",
				__FILE__,__LINE__,info->device_name);
		return -EFAULT;
	}

	spin_lock_irqsave(&info->lock,flags);
	memcpy(&info->params,&tmp_params,sizeof(MGSL_PARAMS));
	spin_unlock_irqrestore(&info->lock,flags);

 	change_params(info);

	return 0;
}

static int get_txidle(SLMP_INFO * info, int*idle_mode)
{
	int err;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s get_txidle()=%d\n",
			 __FILE__,__LINE__, info->device_name, info->idle_mode);

	COPY_TO_USER(err,idle_mode, &info->idle_mode, sizeof(int));
	if (err) {
		if ( debug_level >= DEBUG_LEVEL_INFO )
			printk( "%s(%d):%s get_txidle() user buffer copy failed\n",
				__FILE__,__LINE__,info->device_name);
		return -EFAULT;
	}

	return 0;
}

static int set_txidle(SLMP_INFO * info, int idle_mode)
{
 	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s set_txidle(%d)\n",
			__FILE__,__LINE__,info->device_name, idle_mode );

	spin_lock_irqsave(&info->lock,flags);
	info->idle_mode = idle_mode;
	tx_set_idle( info );
	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

static int tx_enable(SLMP_INFO * info, int enable)
{
 	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s tx_enable(%d)\n",
			__FILE__,__LINE__,info->device_name, enable);

	spin_lock_irqsave(&info->lock,flags);
	if ( enable ) {
		if ( !info->tx_enabled ) {
			tx_start(info);
		}
	} else {
		if ( info->tx_enabled )
			tx_stop(info);
	}
	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

/* abort send HDLC frame
 */
static int tx_abort(SLMP_INFO * info)
{
 	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s tx_abort()\n",
			__FILE__,__LINE__,info->device_name);

	spin_lock_irqsave(&info->lock,flags);
	if ( info->tx_active && info->params.mode == MGSL_MODE_HDLC ) {
		info->ie1_value &= ~UDRN;
		info->ie1_value |= IDLE;
		write_reg(info, IE1, info->ie1_value);	/* disable tx status interrupts */
		write_reg(info, SR1, (unsigned char)(IDLE + UDRN));	/* clear pending */

		write_reg(info, TXDMA + DSR, 0);		/* disable DMA channel */
		write_reg(info, TXDMA + DCMD, SWABORT);	/* reset/init DMA channel */

   		write_reg(info, CMD, TXABORT);
	}
	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

static int rx_enable(SLMP_INFO * info, int enable)
{
 	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s rx_enable(%d)\n",
			__FILE__,__LINE__,info->device_name,enable);

	spin_lock_irqsave(&info->lock,flags);
	if ( enable ) {
		if ( !info->rx_enabled )
			rx_start(info);
	} else {
		if ( info->rx_enabled )
			rx_stop(info);
	}
	spin_unlock_irqrestore(&info->lock,flags);
	return 0;
}

static int map_status(int signals)
{
	/* Map status bits to API event bits */

	return ((signals & SerialSignal_DSR) ? MgslEvent_DsrActive : MgslEvent_DsrInactive) +
	       ((signals & SerialSignal_CTS) ? MgslEvent_CtsActive : MgslEvent_CtsInactive) +
	       ((signals & SerialSignal_DCD) ? MgslEvent_DcdActive : MgslEvent_DcdInactive) +
	       ((signals & SerialSignal_RI)  ? MgslEvent_RiActive : MgslEvent_RiInactive);
}

/* wait for specified event to occur
 */
static int wait_mgsl_event(SLMP_INFO * info, int * mask_ptr)
{
 	unsigned long flags;
	int s;
	int rc=0;
	struct mgsl_icount cprev, cnow;
	int events;
	int mask;
	struct	_input_signal_events oldsigs, newsigs;
	DECLARE_WAITQUEUE(wait, current);

	COPY_FROM_USER(rc,&mask, mask_ptr, sizeof(int));
	if (rc) {
		return  -EFAULT;
	}

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s wait_mgsl_event(%d)\n",
			__FILE__,__LINE__,info->device_name,mask);

	spin_lock_irqsave(&info->lock,flags);

	/* return immediately if state matches requested events */
	get_signals(info);
	s = map_status(info->serial_signals);

	events = mask &
		( ((s & SerialSignal_DSR) ? MgslEvent_DsrActive:MgslEvent_DsrInactive) +
 		  ((s & SerialSignal_DCD) ? MgslEvent_DcdActive:MgslEvent_DcdInactive) +
		  ((s & SerialSignal_CTS) ? MgslEvent_CtsActive:MgslEvent_CtsInactive) +
		  ((s & SerialSignal_RI)  ? MgslEvent_RiActive :MgslEvent_RiInactive) );
	if (events) {
		spin_unlock_irqrestore(&info->lock,flags);
		goto exit;
	}

	/* save current irq counts */
	cprev = info->icount;
	oldsigs = info->input_signal_events;

	/* enable hunt and idle irqs if needed */
	if (mask & (MgslEvent_ExitHuntMode+MgslEvent_IdleReceived)) {
		unsigned char oldval = info->ie1_value;
		unsigned char newval = oldval +
			 (mask & MgslEvent_ExitHuntMode ? FLGD:0) +
			 (mask & MgslEvent_IdleReceived ? IDLE:0);
		if ( oldval != newval ) {
			info->ie1_value = newval;
			write_reg(info, IE1, info->ie1_value);
		}
	}

	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&info->event_wait_q, &wait);

	spin_unlock_irqrestore(&info->lock,flags);

	for(;;) {
		schedule();
		if (signal_pending(current)) {
			rc = -ERESTARTSYS;
			break;
		}

		/* get current irq counts */
		spin_lock_irqsave(&info->lock,flags);
		cnow = info->icount;
		newsigs = info->input_signal_events;
		set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&info->lock,flags);

		/* if no change, wait aborted for some reason */
		if (newsigs.dsr_up   == oldsigs.dsr_up   &&
		    newsigs.dsr_down == oldsigs.dsr_down &&
		    newsigs.dcd_up   == oldsigs.dcd_up   &&
		    newsigs.dcd_down == oldsigs.dcd_down &&
		    newsigs.cts_up   == oldsigs.cts_up   &&
		    newsigs.cts_down == oldsigs.cts_down &&
		    newsigs.ri_up    == oldsigs.ri_up    &&
		    newsigs.ri_down  == oldsigs.ri_down  &&
		    cnow.exithunt    == cprev.exithunt   &&
		    cnow.rxidle      == cprev.rxidle) {
			rc = -EIO;
			break;
		}

		events = mask &
			( (newsigs.dsr_up   != oldsigs.dsr_up   ? MgslEvent_DsrActive:0)   +
			  (newsigs.dsr_down != oldsigs.dsr_down ? MgslEvent_DsrInactive:0) +
			  (newsigs.dcd_up   != oldsigs.dcd_up   ? MgslEvent_DcdActive:0)   +
			  (newsigs.dcd_down != oldsigs.dcd_down ? MgslEvent_DcdInactive:0) +
			  (newsigs.cts_up   != oldsigs.cts_up   ? MgslEvent_CtsActive:0)   +
			  (newsigs.cts_down != oldsigs.cts_down ? MgslEvent_CtsInactive:0) +
			  (newsigs.ri_up    != oldsigs.ri_up    ? MgslEvent_RiActive:0)    +
			  (newsigs.ri_down  != oldsigs.ri_down  ? MgslEvent_RiInactive:0)  +
			  (cnow.exithunt    != cprev.exithunt   ? MgslEvent_ExitHuntMode:0) +
			  (cnow.rxidle      != cprev.rxidle     ? MgslEvent_IdleReceived:0) );
		if (events)
			break;

		cprev = cnow;
		oldsigs = newsigs;
	}

	remove_wait_queue(&info->event_wait_q, &wait);
	set_current_state(TASK_RUNNING);


	if (mask & (MgslEvent_ExitHuntMode + MgslEvent_IdleReceived)) {
		spin_lock_irqsave(&info->lock,flags);
		if (!waitqueue_active(&info->event_wait_q)) {
			/* disable enable exit hunt mode/idle rcvd IRQs */
			info->ie1_value &= ~(FLGD|IDLE);
			write_reg(info, IE1, info->ie1_value);
		}
		spin_unlock_irqrestore(&info->lock,flags);
	}
exit:
	if ( rc == 0 )
		PUT_USER(rc, events, mask_ptr);

	return rc;
}

static int modem_input_wait(SLMP_INFO *info,int arg)
{
 	unsigned long flags;
	int rc;
	struct mgsl_icount cprev, cnow;
	DECLARE_WAITQUEUE(wait, current);

	/* save current irq counts */
	spin_lock_irqsave(&info->lock,flags);
	cprev = info->icount;
	add_wait_queue(&info->status_event_wait_q, &wait);
	set_current_state(TASK_INTERRUPTIBLE);
	spin_unlock_irqrestore(&info->lock,flags);

	for(;;) {
		schedule();
		if (signal_pending(current)) {
			rc = -ERESTARTSYS;
			break;
		}

		/* get new irq counts */
		spin_lock_irqsave(&info->lock,flags);
		cnow = info->icount;
		set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&info->lock,flags);

		/* if no change, wait aborted for some reason */
		if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr &&
		    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts) {
			rc = -EIO;
			break;
		}

		/* check for change in caller specified modem input */
		if ((arg & TIOCM_RNG && cnow.rng != cprev.rng) ||
		    (arg & TIOCM_DSR && cnow.dsr != cprev.dsr) ||
		    (arg & TIOCM_CD  && cnow.dcd != cprev.dcd) ||
		    (arg & TIOCM_CTS && cnow.cts != cprev.cts)) {
			rc = 0;
			break;
		}

		cprev = cnow;
	}
	remove_wait_queue(&info->status_event_wait_q, &wait);
	set_current_state(TASK_RUNNING);
	return rc;
}

/* return the state of the serial control and status signals
 */
static int get_modem_info(SLMP_INFO * info, unsigned int *value)
{
	unsigned int result;
 	unsigned long flags;
	int err;

	spin_lock_irqsave(&info->lock,flags);
 	get_signals(info);
	spin_unlock_irqrestore(&info->lock,flags);

	result = ((info->serial_signals & SerialSignal_RTS) ? TIOCM_RTS:0) +
		((info->serial_signals & SerialSignal_DTR) ? TIOCM_DTR:0) +
		((info->serial_signals & SerialSignal_DCD) ? TIOCM_CAR:0) +
		((info->serial_signals & SerialSignal_RI)  ? TIOCM_RNG:0) +
		((info->serial_signals & SerialSignal_DSR) ? TIOCM_DSR:0) +
		((info->serial_signals & SerialSignal_CTS) ? TIOCM_CTS:0);

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s synclinkmp_get_modem_info() value=%08X\n",
			 __FILE__,__LINE__, info->device_name, result );

	PUT_USER(err,result,value);
	return err;
}

/* set modem control signals (DTR/RTS)
 *
 * 	cmd	signal command: TIOCMBIS = set bit TIOCMBIC = clear bit
 *		TIOCMSET = set/clear signal values
 * 	value	bit mask for command
 */
static int set_modem_info(SLMP_INFO * info, unsigned int cmd,
			  unsigned int *value)
{
 	int error;
 	unsigned int arg;
 	unsigned long flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s synclinkmp_set_modem_info()\n",
			__FILE__,__LINE__,info->device_name );

 	GET_USER(error,arg,value);
 	if (error)
 		return error;

 	switch (cmd) {
 	case TIOCMBIS:
 		if (arg & TIOCM_RTS)
 			info->serial_signals |= SerialSignal_RTS;
 		if (arg & TIOCM_DTR)
 			info->serial_signals |= SerialSignal_DTR;
 		break;
 	case TIOCMBIC:
 		if (arg & TIOCM_RTS)
 			info->serial_signals &= ~SerialSignal_RTS;
 		if (arg & TIOCM_DTR)
 			info->serial_signals &= ~SerialSignal_DTR;
 		break;
 	case TIOCMSET:
 		if (arg & TIOCM_RTS)
 			info->serial_signals |= SerialSignal_RTS;
		else
 			info->serial_signals &= ~SerialSignal_RTS;

 		if (arg & TIOCM_DTR)
 			info->serial_signals |= SerialSignal_DTR;
		else
 			info->serial_signals &= ~SerialSignal_DTR;
 		break;
 	default:
 		return -EINVAL;
 	}

	spin_lock_irqsave(&info->lock,flags);
 	set_signals(info);
	spin_unlock_irqrestore(&info->lock,flags);

	return 0;
}



/* Block the current process until the specified port is ready to open.
 */
static int block_til_ready(struct tty_struct *tty, struct file *filp,
			   SLMP_INFO *info)
{
	DECLARE_WAITQUEUE(wait, current);
	int		retval;
	int		do_clocal = 0, extra_count = 0;
	unsigned long	flags;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s block_til_ready()\n",
			 __FILE__,__LINE__, tty->driver.name );

	if (tty->driver.subtype == SERIAL_TYPE_CALLOUT) {
		/* this is a callout device */
		/* just verify that normal device is not in use */
		if (info->flags & ASYNC_NORMAL_ACTIVE)
			return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_SESSION_LOCKOUT) &&
		    (info->session != current->session))
		    return -EBUSY;
		if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
		    (info->flags & ASYNC_PGRP_LOCKOUT) &&
		    (info->pgrp != current->pgrp))
		    return -EBUSY;
		info->flags |= ASYNC_CALLOUT_ACTIVE;
		return 0;
	}

	if (filp->f_flags & O_NONBLOCK || tty->flags & (1 << TTY_IO_ERROR)){
		/* nonblock mode is set or port is not enabled */
		/* just verify that callout device is not active */
		if (info->flags & ASYNC_CALLOUT_ACTIVE)
			return -EBUSY;
		info->flags |= ASYNC_NORMAL_ACTIVE;
		return 0;
	}

	if (info->flags & ASYNC_CALLOUT_ACTIVE) {
		if (info->normal_termios.c_cflag & CLOCAL)
			do_clocal = 1;
	} else {
		if (tty->termios->c_cflag & CLOCAL)
			do_clocal = 1;
	}

	/* Wait for carrier detect and the line to become
	 * free (i.e., not in use by the callout).  While we are in
	 * this loop, info->count is dropped by one, so that
	 * close() knows when to free things.  We restore it upon
	 * exit, either normal or abnormal.
	 */

	retval = 0;
	add_wait_queue(&info->open_wait, &wait);

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s block_til_ready() before block, count=%d\n",
			 __FILE__,__LINE__, tty->driver.name, info->count );

	save_flags(flags); cli();
	if (!tty_hung_up_p(filp)) {
		extra_count = 1;
		info->count--;
	}
	restore_flags(flags);
	info->blocked_open++;

	while (1) {
		if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
 		    (tty->termios->c_cflag & CBAUD)) {
			spin_lock_irqsave(&info->lock,flags);
			info->serial_signals |= SerialSignal_RTS + SerialSignal_DTR;
		 	set_signals(info);
			spin_unlock_irqrestore(&info->lock,flags);
		}

		set_current_state(TASK_INTERRUPTIBLE);

		if (tty_hung_up_p(filp) || !(info->flags & ASYNC_INITIALIZED)){
			retval = (info->flags & ASYNC_HUP_NOTIFY) ?
					-EAGAIN : -ERESTARTSYS;
			break;
		}

		spin_lock_irqsave(&info->lock,flags);
	 	get_signals(info);
		spin_unlock_irqrestore(&info->lock,flags);

 		if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
 		    !(info->flags & ASYNC_CLOSING) &&
 		    (do_clocal || (info->serial_signals & SerialSignal_DCD)) ) {
 			break;
		}

		if (signal_pending(current)) {
			retval = -ERESTARTSYS;
			break;
		}

		if (debug_level >= DEBUG_LEVEL_INFO)
			printk("%s(%d):%s block_til_ready() count=%d\n",
				 __FILE__,__LINE__, tty->driver.name, info->count );

		schedule();
	}

	set_current_state(TASK_RUNNING);
	remove_wait_queue(&info->open_wait, &wait);

	if (extra_count)
		info->count++;
	info->blocked_open--;

	if (debug_level >= DEBUG_LEVEL_INFO)
		printk("%s(%d):%s block_til_ready() after, count=%d\n",
			 __FILE__,__LINE__, tty->driver.name, info->count );

	if (!retval)
		info->flags |= ASYNC_NORMAL_ACTIVE;

	return retval;
}

int alloc_dma_bufs(SLMP_INFO *info)
{
	unsigned short BuffersPerFrame;
	unsigned short BufferCount;

	// Force allocation to start at 64K boundary for each port.
	// This is necessary because *all* buffer descriptors for a port
	// *must* be in the same 64K block. All descriptors on a port
	// share a common 'base' address (upper 8 bits of 24 bits) programmed
	// into the CBP register.
	info->port_array[0]->last_mem_alloc = (SCA_MEM_SIZE/4) * info->port_num;

	/* Calculate the number of DMA buffers necessary to hold the */
	/* largest allowable frame size. Note: If the max frame size is */
	/* not an even multiple of the DMA buffer size then we need to */
	/* round the buffer count per frame up one. */

	BuffersPerFrame = (unsigned short)(info->max_frame_size/SCABUFSIZE);
	if ( info->max_frame_size % SCABUFSIZE )
		BuffersPerFrame++;

	/* calculate total number of data buffers (SCABUFSIZE) possible
	 * in one ports memory (SCA_MEM_SIZE/4) after allocating memory
	 * for the descriptor list (BUFFERLISTSIZE).
	 */
	BufferCount = (SCA_MEM_SIZE/4 - BUFFERLISTSIZE)/SCABUFSIZE;

	/* limit number of buffers to maximum amount of descriptors */
	if (BufferCount > BUFFERLISTSIZE/sizeof(SCADESC))
		BufferCount = BUFFERLISTSIZE/sizeof(SCADESC);

	/* use enough buffers to transmit one max size frame */
	info->tx_buf_count = BuffersPerFrame + 1;

	/* never use more than half the available buffers for transmit */
	if (info->tx_buf_count > (BufferCount/2))
		info->tx_buf_count = BufferCount/2;

	if (info->tx_buf_count > SCAMAXDESC)
		info->tx_buf_count = SCAMAXDESC;

	/* use remaining buffers for receive */
	info->rx_buf_count = BufferCount - info->tx_buf_count;

	if (info->rx_buf_count > SCAMAXDESC)
		info->rx_buf_count = SCAMAXDESC;

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk("%s(%d):%s Allocating %d TX and %d RX DMA buffers.\n",
			__FILE__,__LINE__, info->device_name,
			info->tx_buf_count,info->rx_buf_count);

	if ( alloc_buf_list( info ) < 0 ||
		alloc_frame_bufs(info,
		  			info->rx_buf_list,
		  			info->rx_buf_list_ex,
					info->rx_buf_count) < 0 ||
		alloc_frame_bufs(info,
					info->tx_buf_list,
					info->tx_buf_list_ex,
					info->tx_buf_count) < 0 ||
		alloc_tmp_rx_buf(info) < 0 ) {
		printk("%s(%d):%s Can't allocate DMA buffer memory\n",
			__FILE__,__LINE__, info->device_name);
		return -ENOMEM;
	}

	rx_reset_buffers( info );

	return 0;
}

/* Allocate DMA buffers for the transmit and receive descriptor lists.
 */
int alloc_buf_list(SLMP_INFO *info)
{
	unsigned int i;

	/* build list in adapter shared memory */
	info->buffer_list = info->memory_base + info->port_array[0]->last_mem_alloc;
	info->buffer_list_phys = info->port_array[0]->last_mem_alloc;
	info->port_array[0]->last_mem_alloc += BUFFERLISTSIZE;

	memset(info->buffer_list, 0, BUFFERLISTSIZE);

	/* Save virtual address pointers to the receive and */
	/* transmit buffer lists. (Receive 1st). These pointers will */
	/* be used by the processor to access the lists. */
	info->rx_buf_list = (SCADESC *)info->buffer_list;

	info->tx_buf_list = (SCADESC *)info->buffer_list;
	info->tx_buf_list += info->rx_buf_count;

	/* Build links for circular buffer entry lists (tx and rx)
	 *
	 * Note: links are physical addresses read by the SCA device
	 * to determine the next buffer entry to use.
	 */

	for ( i = 0; i < info->rx_buf_count; i++ ) {
		/* calculate and store physical address of this buffer entry */
		info->rx_buf_list_ex[i].phys_entry =
			info->buffer_list_phys + (i * sizeof(SCABUFSIZE));

		/* calculate and store physical address of */
		/* next entry in cirular list of entries */
		info->rx_buf_list[i].next = info->buffer_list_phys;
		if ( i < info->rx_buf_count - 1 )
			info->rx_buf_list[i].next += (i + 1) * sizeof(SCADESC);

		info->rx_buf_list[i].length = SCABUFSIZE;
	}

	for ( i = 0; i < info->tx_buf_count; i++ ) {
		/* calculate and store physical address of this buffer entry */
		info->tx_buf_list_ex[i].phys_entry = info->buffer_list_phys +
			((info->rx_buf_count + i) * sizeof(SCADESC));

		/* calculate and store physical address of */
		/* next entry in cirular list of entries */

		info->tx_buf_list[i].next = info->buffer_list_phys +
			info->rx_buf_count * sizeof(SCADESC);

		if ( i < info->tx_buf_count - 1 )
			info->tx_buf_list[i].next += (i + 1) * sizeof(SCADESC);
	}

	return 0;
}

/* Allocate the frame DMA buffers used by the specified buffer list.
 */
int alloc_frame_bufs(SLMP_INFO *info, SCADESC *buf_list,SCADESC_EX *buf_list_ex,int count)
{
	int i;
	unsigned long phys_addr;

	for ( i = 0; i < count; i++ ) {
		buf_list_ex[i].virt_addr = info->memory_base + info->port_array[0]->last_mem_alloc;
		phys_addr = info->port_array[0]->last_mem_alloc;
		info->port_array[0]->last_mem_alloc += SCABUFSIZE;

		buf_list[i].buf_ptr  = (unsigned short)phys_addr;
		buf_list[i].buf_base = (unsigned char)(phys_addr >> 16);
	}

	return 0;
}

void free_dma_bufs(SLMP_INFO *info)
{
	info->buffer_list = NULL;
	info->rx_buf_list = NULL;
	info->tx_buf_list = NULL;
}

/* allocate buffer large enough to hold max_frame_size.
 * This buffer is used to pass an assembled frame to the line discipline.
 */
int alloc_tmp_rx_buf(SLMP_INFO *info)
{
	info->tmp_rx_buf = kmalloc(info->max_frame_size, GFP_KERNEL);
	if (info->tmp_rx_buf == NULL)
		return -ENOMEM;
	return 0;
}

void free_tmp_rx_buf(SLMP_INFO *info)
{
	if (info->tmp_rx_buf)
		kfree(info->tmp_rx_buf);
	info->tmp_rx_buf = NULL;
}

int claim_resources(SLMP_INFO *info)
{
	if (request_mem_region(info->phys_memory_base,0x40000,"synclinkmp") == NULL) {
		printk( "%s(%d):%s mem addr conflict, Addr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->phys_memory_base);
		goto errout;
	}
	else
		info->shared_mem_requested = 1;

	if (request_mem_region(info->phys_lcr_base + info->lcr_offset,128,"synclinkmp") == NULL) {
		printk( "%s(%d):%s lcr mem addr conflict, Addr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->phys_lcr_base);
		goto errout;
	}
	else
		info->lcr_mem_requested = 1;

	if (request_mem_region(info->phys_sca_base + info->sca_offset,512,"synclinkmp") == NULL) {
		printk( "%s(%d):%s sca mem addr conflict, Addr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->phys_sca_base);
		goto errout;
	}
	else
		info->sca_base_requested = 1;

	if (request_mem_region(info->phys_statctrl_base + info->statctrl_offset,16,"synclinkmp") == NULL) {
		printk( "%s(%d):%s stat/ctrl mem addr conflict, Addr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->phys_statctrl_base);
		goto errout;
	}
	else
		info->sca_statctrl_requested = 1;

	info->memory_base = ioremap(info->phys_memory_base,SCA_MEM_SIZE);
	if (!info->memory_base) {
		printk( "%s(%d):%s Cant map shared memory, MemAddr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->phys_memory_base );
		goto errout;
	}

	if ( !memory_test(info) ) {
		printk( "%s(%d):Shared Memory Test failed for device %s MemAddr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->phys_memory_base );
		goto errout;
	}

	info->lcr_base = ioremap(info->phys_lcr_base,PAGE_SIZE) + info->lcr_offset;
	if (!info->lcr_base) {
		printk( "%s(%d):%s Cant map LCR memory, MemAddr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->phys_lcr_base );
		goto errout;
	}

	info->sca_base = ioremap(info->phys_sca_base,PAGE_SIZE) + info->sca_offset;
	if (!info->sca_base) {
		printk( "%s(%d):%s Cant map SCA memory, MemAddr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->phys_sca_base );
		goto errout;
	}

	info->statctrl_base = ioremap(info->phys_statctrl_base,PAGE_SIZE) + info->statctrl_offset;
	if (!info->statctrl_base) {
		printk( "%s(%d):%s Cant map SCA Status/Control memory, MemAddr=%08X\n",
			__FILE__,__LINE__,info->device_name, info->phys_statctrl_base );
		goto errout;
	}

	return 0;

errout:
	release_resources( info );
	return -ENODEV;
}

void release_resources(SLMP_INFO *info)
{
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):%s release_resources() entry\n",
			__FILE__,__LINE__,info->device_name );

	if ( info->irq_requested ) {
		free_irq(info->irq_level, info);
		info->irq_requested = 0;
	}

	if ( info->shared_mem_requested ) {
		release_mem_region(info->phys_memory_base,0x40000);
		info->shared_mem_requested = 0;
	}
	if ( info->lcr_mem_requested ) {
		release_mem_region(info->phys_lcr_base + info->lcr_offset,128);
		info->lcr_mem_requested = 0;
	}
	if ( info->sca_base_requested ) {
		release_mem_region(info->phys_sca_base + info->sca_offset,512);
		info->sca_base_requested = 0;
	}
	if ( info->sca_statctrl_requested ) {
		release_mem_region(info->phys_statctrl_base + info->statctrl_offset,16);
		info->sca_statctrl_requested = 0;
	}

	if (info->memory_base){
		iounmap(info->memory_base);
		info->memory_base = 0;
	}

	if (info->sca_base) {
		iounmap(info->sca_base - info->sca_offset);
		info->sca_base=0;
	}

	if (info->statctrl_base) {
		iounmap(info->statctrl_base - info->statctrl_offset);
		info->statctrl_base=0;
	}

	if (info->lcr_base){
		iounmap(info->lcr_base - info->lcr_offset);
		info->lcr_base = 0;
	}

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):%s release_resources() exit\n",
			__FILE__,__LINE__,info->device_name );
}

/* Add the specified device instance data structure to the
 * global linked list of devices and increment the device count.
 */
void add_device(SLMP_INFO *info)
{
	info->next_device = NULL;
	info->line = synclinkmp_device_count;
	sprintf(info->device_name,"ttySLM%dp%d",info->adapter_num,info->port_num);

	if (info->line < MAX_DEVICES) {
		if (maxframe[info->line])
			info->max_frame_size = maxframe[info->line];
		info->dosyncppp = dosyncppp[info->line];
	}

	synclinkmp_device_count++;

	if ( !synclinkmp_device_list )
		synclinkmp_device_list = info;
	else {
		SLMP_INFO *current_dev = synclinkmp_device_list;
		while( current_dev->next_device )
			current_dev = current_dev->next_device;
		current_dev->next_device = info;
	}

	if ( info->max_frame_size < 4096 )
		info->max_frame_size = 4096;
	else if ( info->max_frame_size > 65535 )
		info->max_frame_size = 65535;

	printk( "SyncLink MultiPort %s: "
		"Mem=(%08x %08X %08x %08X) IRQ=%d MaxFrameSize=%u\n",
		info->device_name,
		info->phys_sca_base,
		info->phys_memory_base,
		info->phys_statctrl_base,
		info->phys_lcr_base,
		info->irq_level,
		info->max_frame_size );

#ifdef CONFIG_SYNCLINK_SYNCPPP
	if (info->dosyncppp)
		sppp_init(info);
#endif
}

/* Allocate and initialize a device instance structure
 *
 * Return Value:	pointer to SLMP_INFO if success, otherwise NULL
 */
SLMP_INFO *alloc_dev(int adapter_num, int port_num, struct pci_dev *pdev)
{
	SLMP_INFO *info;

	info = (SLMP_INFO *)kmalloc(sizeof(SLMP_INFO),
		 GFP_KERNEL);

	if (!info) {
		printk("%s(%d) Error can't allocate device instance data for adapter %d, port %d\n",
			__FILE__,__LINE__, adapter_num, port_num);
	} else {
		memset(info, 0, sizeof(SLMP_INFO));
		info->magic = MGSL_MAGIC;
		info->task.sync = 0;
		info->task.routine = bh_handler;
		info->task.data    = info;
		info->max_frame_size = 4096;
		info->close_delay = 5*HZ/10;
		info->closing_wait = 30*HZ;
		init_waitqueue_head(&info->open_wait);
		init_waitqueue_head(&info->close_wait);
		init_waitqueue_head(&info->status_event_wait_q);
		init_waitqueue_head(&info->event_wait_q);
		spin_lock_init(&info->netlock);
		memcpy(&info->params,&default_params,sizeof(MGSL_PARAMS));
		info->idle_mode = HDLC_TXIDLE_FLAGS;
		info->adapter_num = adapter_num;
		info->port_num = port_num;

		/* Copy configuration info to device instance data */
		info->irq_level = pdev->irq;
		info->phys_lcr_base = pci_resource_start(pdev,0);
		info->phys_sca_base = pci_resource_start(pdev,2);
		info->phys_memory_base = pci_resource_start(pdev,3);
		info->phys_statctrl_base = pci_resource_start(pdev,4);

		/* Because veremap only works on page boundaries we must map
		 * a larger area than is actually implemented for the LCR
		 * memory range. We map a full page starting at the page boundary.
		 */
		info->lcr_offset    = info->phys_lcr_base & (PAGE_SIZE-1);
		info->phys_lcr_base &= ~(PAGE_SIZE-1);

		info->sca_offset    = info->phys_sca_base & (PAGE_SIZE-1);
		info->phys_sca_base &= ~(PAGE_SIZE-1);

		info->statctrl_offset    = info->phys_statctrl_base & (PAGE_SIZE-1);
		info->phys_statctrl_base &= ~(PAGE_SIZE-1);

		info->bus_type = MGSL_BUS_TYPE_PCI;
		info->irq_flags = SA_SHIRQ;

		/* Store the PCI9050 misc control register value because a flaw
		 * in the PCI9050 prevents LCR registers from being read if
		 * BIOS assigns an LCR base address with bit 7 set.
		 *
		 * Only the misc control register is accessed for which only
		 * write access is needed, so set an initial value and change
		 * bits to the device instance data as we write the value
		 * to the actual misc control register.
		 */
		info->misc_ctrl_value = 0x087e4546;

		/* initial port state is unknown - if startup errors
		 * occur, init_error will be set to indicate the
		 * problem. Once the port is fully initialized,
		 * this value will be set to 0 to indicate the
		 * port is available.
		 */
		info->init_error = -1;
	}

	return info;
}

void device_init(int adapter_num, struct pci_dev *pdev)
{
	SLMP_INFO *port_array[SCA_MAX_PORTS];
	int port;

	/* allocate device instances for up to SCA_MAX_PORTS devices */
	for ( port = 0; port < SCA_MAX_PORTS; ++port ) {
		port_array[port] = alloc_dev(adapter_num,port,pdev);
		if( port_array[port] == NULL ) {
			for ( --port; port >= 0; --port )
				kfree(port_array[port]);
			return;
		}
	}

	/* give copy of port_array to all ports and add to device list  */
	for ( port = 0; port < SCA_MAX_PORTS; ++port ) {
		memcpy(port_array[port]->port_array,port_array,sizeof(port_array));
		add_device( port_array[port] );
		spin_lock_init(&port_array[port]->lock);
	}

	/* Allocate and claim adapter resources */
	if ( !claim_resources(port_array[0]) ) {

		alloc_dma_bufs(port_array[0]);

		/* copy resource information from first port to others */
		for ( port = 1; port < SCA_MAX_PORTS; ++port ) {
			port_array[port]->lock  = port_array[0]->lock;
			port_array[port]->irq_level     = port_array[0]->irq_level;
			port_array[port]->memory_base   = port_array[0]->memory_base;
			port_array[port]->sca_base      = port_array[0]->sca_base;
			port_array[port]->statctrl_base = port_array[0]->statctrl_base;
			port_array[port]->lcr_base      = port_array[0]->lcr_base;
			alloc_dma_bufs(port_array[port]);
		}

		if ( request_irq(port_array[0]->irq_level,
					synclinkmp_interrupt,
					port_array[0]->irq_flags,
					port_array[0]->device_name,
					port_array[0]) < 0 ) {
			printk( "%s(%d):%s Cant request interrupt, IRQ=%d\n",
				__FILE__,__LINE__,
				port_array[0]->device_name,
				port_array[0]->irq_level );
		}
		else {
			port_array[0]->irq_requested = 1;
			adapter_test(port_array[0]);
		}
	}
}

/* Driver initialization entry point.
 */

static int __init synclinkmp_init(void)
{
	SLMP_INFO *info;

	EXPORT_NO_SYMBOLS;

	if (break_on_load) {
	 	synclinkmp_get_text_ptr();
  		BREAKPOINT();
	}

 	printk("%s %s\n", driver_name, driver_version);

	synclinkmp_adapter_count = -1;
	pci_register_driver(&synclinkmp_pci_driver);

	if ( !synclinkmp_device_list ) {
		printk("%s(%d):No SyncLink devices found.\n",__FILE__,__LINE__);
		return -ENODEV;
	}

	memset(serial_table,0,sizeof(struct tty_struct*)*MAX_DEVICES);
	memset(serial_termios,0,sizeof(struct termios*)*MAX_DEVICES);
	memset(serial_termios_locked,0,sizeof(struct termios*)*MAX_DEVICES);

	/* Initialize the tty_driver structure */

	memset(&serial_driver, 0, sizeof(struct tty_driver));
	serial_driver.magic = TTY_DRIVER_MAGIC;
	serial_driver.driver_name = "synclinkmp";
	serial_driver.name = "ttySLM";
	serial_driver.major = ttymajor;
	serial_driver.minor_start = 64;
	serial_driver.num = synclinkmp_device_count;
	serial_driver.type = TTY_DRIVER_TYPE_SERIAL;
	serial_driver.subtype = SERIAL_TYPE_NORMAL;
	serial_driver.init_termios = tty_std_termios;
	serial_driver.init_termios.c_cflag =
		B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	serial_driver.flags = TTY_DRIVER_REAL_RAW;
	serial_driver.refcount = &serial_refcount;
	serial_driver.table = serial_table;
	serial_driver.termios = serial_termios;
	serial_driver.termios_locked = serial_termios_locked;

	serial_driver.open = open;
	serial_driver.close = close;
	serial_driver.write = write;
	serial_driver.put_char = put_char;
	serial_driver.flush_chars = flush_chars;
	serial_driver.write_room = write_room;
	serial_driver.chars_in_buffer = chars_in_buffer;
	serial_driver.flush_buffer = flush_buffer;
	serial_driver.ioctl = ioctl;
	serial_driver.throttle = throttle;
	serial_driver.unthrottle = unthrottle;
	serial_driver.send_xchar = send_xchar;
	serial_driver.break_ctl = set_break;
	serial_driver.wait_until_sent = wait_until_sent;
 	serial_driver.read_proc = read_proc;
	serial_driver.set_termios = set_termios;
	serial_driver.stop = tx_hold;
	serial_driver.start = tx_release;
	serial_driver.hangup = hangup;

	/*
	 * The callout device is just like normal device except for
	 * major number and the subtype code.
	 */
	callout_driver = serial_driver;
	callout_driver.name = "cuaSLM";
	callout_driver.major = cuamajor;
	callout_driver.subtype = SERIAL_TYPE_CALLOUT;
	callout_driver.read_proc = 0;
	callout_driver.proc_entry = 0;

	if (tty_register_driver(&serial_driver) < 0)
		printk("%s(%d):Couldn't register serial driver\n",
			__FILE__,__LINE__);

	if (tty_register_driver(&callout_driver) < 0)
		printk("%s(%d):Couldn't register callout driver\n",
			__FILE__,__LINE__);

 	printk("%s %s, tty major#%d callout major#%d\n",
		driver_name, driver_version,
		serial_driver.major, callout_driver.major);

	/* Propagate these values to all device instances */

	info = synclinkmp_device_list;
	while(info){
		info->callout_termios = callout_driver.init_termios;
		info->normal_termios  = serial_driver.init_termios;
		info = info->next_device;
	}

	return 0;
}

static void __exit synclinkmp_exit(void)
{
	unsigned long flags;
	int rc;
	SLMP_INFO *info;
	SLMP_INFO *tmp;

	printk("Unloading %s %s\n", driver_name, driver_version);
	save_flags(flags);
	cli();
	if ((rc = tty_unregister_driver(&serial_driver)))
		printk("%s(%d) failed to unregister tty driver err=%d\n",
		       __FILE__,__LINE__,rc);
	if ((rc = tty_unregister_driver(&callout_driver)))
		printk("%s(%d) failed to unregister callout driver err=%d\n",
		       __FILE__,__LINE__,rc);
	restore_flags(flags);

	info = synclinkmp_device_list;
	while(info) {
#ifdef CONFIG_SYNCLINK_SYNCPPP
		if (info->dosyncppp)
			sppp_delete(info);
#endif
		reset_port(info);
		if ( info->port_num == 0 ) {
			if ( info->irq_requested ) {
				free_irq(info->irq_level, info);
				info->irq_requested = 0;
			}
		}
		info = info->next_device;
	}

	/* port 0 of each adapter originally claimed
	 * all resources, release those now
	 */
	info = synclinkmp_device_list;
	while(info) {
		free_dma_bufs(info);
		free_tmp_rx_buf(info);
		if ( info->port_num == 0 ) {
			spin_lock_irqsave(&info->lock,flags);
			reset_adapter(info);
			write_reg(info, LPR, 1);		/* set low power mode */
			spin_unlock_irqrestore(&info->lock,flags);
			release_resources(info);
		}
		tmp = info;
		info = info->next_device;
		kfree(tmp);
	}

	pci_unregister_driver(&synclinkmp_pci_driver);
}

module_init(synclinkmp_init);
module_exit(synclinkmp_exit);

/* Set the port for internal loopback mode.
 * The TxCLK and RxCLK signals are generated from the BRG and
 * the TxD is looped back to the RxD internally.
 */
void enable_loopback(SLMP_INFO *info, int enable)
{
	if (enable) {
		/* MD2 (Mode Register 2)
		 * 01..00  CNCT<1..0> Channel Connection 11=Local Loopback
		 */
		write_reg(info, MD2, (unsigned char)(read_reg(info, MD2) | (BIT1 + BIT0)));

		/* degate external TxC clock source */
		info->port_array[0]->ctrlreg_value |= (BIT0 << (info->port_num * 2));
		write_control_reg(info);

		/* RXS/TXS (Rx/Tx clock source)
		 * 07      Reserved, must be 0
		 * 06..04  Clock Source, 100=BRG
		 * 03..00  Clock Divisor, 0000=1
		 */
		write_reg(info, RXS, 0x40);
		write_reg(info, TXS, 0x40);

	} else {
		/* MD2 (Mode Register 2)
	 	 * 01..00  CNCT<1..0> Channel connection, 0=normal
		 */
		write_reg(info, MD2, (unsigned char)(read_reg(info, MD2) & ~(BIT1 + BIT0)));

		/* RXS/TXS (Rx/Tx clock source)
		 * 07      Reserved, must be 0
		 * 06..04  Clock Source, 000=RxC/TxC Pin
		 * 03..00  Clock Divisor, 0000=1
		 */
		write_reg(info, RXS, 0x00);
		write_reg(info, TXS, 0x00);
	}

	/* set LinkSpeed if available, otherwise default to 2Mbps */
	if (info->params.clock_speed)
		set_rate(info, info->params.clock_speed);
	else
		set_rate(info, 3686400);
}

/* Set the baud rate register to the desired speed
 *
 *	data_rate	data rate of clock in bits per second
 *			A data rate of 0 disables the AUX clock.
 */
void set_rate( SLMP_INFO *info, u32 data_rate )
{
       	u32 TMCValue;
       	unsigned char BRValue;
	u32 Divisor=0;

	/* fBRG = fCLK/(TMC * 2^BR)
	 */
	if (data_rate != 0) {
		Divisor = 14745600/data_rate;
		if (!Divisor)
			Divisor = 1;

		TMCValue = Divisor;

		BRValue = 0;
		if (TMCValue != 1 && TMCValue != 2) {
			/* BRValue of 0 provides 50/50 duty cycle *only* when
			 * TMCValue is 1 or 2. BRValue of 1 to 9 always provides
			 * 50/50 duty cycle.
			 */
			BRValue = 1;
			TMCValue >>= 1;
		}

		/* while TMCValue is too big for TMC register, divide
		 * by 2 and increment BR exponent.
		 */
		for(; TMCValue > 256 && BRValue < 10; BRValue++)
			TMCValue >>= 1;

		write_reg(info, TXS,
			(unsigned char)((read_reg(info, TXS) & 0xf0) | BRValue));
		write_reg(info, RXS,
			(unsigned char)((read_reg(info, RXS) & 0xf0) | BRValue));
		write_reg(info, TMC, (unsigned char)TMCValue);
	}
	else {
		write_reg(info, TXS,0);
		write_reg(info, RXS,0);
		write_reg(info, TMC, 0);
	}
}

/* Disable receiver
 */
void rx_stop(SLMP_INFO *info)
{
	if (debug_level >= DEBUG_LEVEL_ISR)
		printk("%s(%d):%s rx_stop()\n",
			 __FILE__,__LINE__, info->device_name );

	write_reg(info, CMD, RXRESET);

	info->ie0_value &= ~RXRDYE;
	write_reg(info, IE0, info->ie0_value);	/* disable Rx data interrupts */

	write_reg(info, RXDMA + DSR, 0);	/* disable Rx DMA */
	write_reg(info, RXDMA + DCMD, SWABORT);	/* reset/init Rx DMA */
	write_reg(info, RXDMA + DIR, 0);	/* disable Rx DMA interrupts */

	info->rx_enabled = 0;
	info->rx_overflow = 0;
}

/* enable the receiver
 */
void rx_start(SLMP_INFO *info)
{
	int i;

	if (debug_level >= DEBUG_LEVEL_ISR)
		printk("%s(%d):%s rx_start()\n",
			 __FILE__,__LINE__, info->device_name );

	write_reg(info, CMD, RXRESET);

	if ( info->params.mode == MGSL_MODE_HDLC ) {
		/* HDLC, disabe IRQ on rxdata */
		info->ie0_value &= ~RXRDYE;
		write_reg(info, IE0, info->ie0_value);

		/* Reset all Rx DMA buffers and program rx dma */
		write_reg(info, RXDMA + DSR, 0);		/* disable Rx DMA */
		write_reg(info, RXDMA + DCMD, SWABORT);	/* reset/init Rx DMA */

		for (i = 0; i < info->rx_buf_count; i++) {
			info->rx_buf_list[i].status = 0xff;

			// throttle to 4 shared memory writes at a time to prevent
			// hogging local bus (keep latency time for DMA requests low).
			if (!(i % 4))
				read_status_reg(info);
		}
		info->current_rx_buf = 0;

		/* set current/1st descriptor address */
		write_reg16(info, RXDMA + CDA,
			info->rx_buf_list_ex[0].phys_entry);

		/* set new last rx descriptor address */
		write_reg16(info, RXDMA + EDA,
			info->rx_buf_list_ex[info->rx_buf_count - 1].phys_entry);

		/* set buffer length (shared by all rx dma data buffers) */
		write_reg16(info, RXDMA + BFL, SCABUFSIZE);

		write_reg(info, RXDMA + DIR, 0x60);	/* enable Rx DMA interrupts (EOM/BOF) */
		write_reg(info, RXDMA + DSR, 0xf2);	/* clear Rx DMA IRQs, enable Rx DMA */
	} else {
		/* async, enable IRQ on rxdata */
		info->ie0_value |= RXRDYE;
		write_reg(info, IE0, info->ie0_value);
	}

	write_reg(info, CMD, RXENABLE);

	info->rx_overflow = FALSE;
	info->rx_enabled = 1;
}

/* Enable the transmitter and send a transmit frame if
 * one is loaded in the DMA buffers.
 */
void tx_start(SLMP_INFO *info)
{
	if (debug_level >= DEBUG_LEVEL_ISR)
		printk("%s(%d):%s tx_start() tx_count=%d\n",
			 __FILE__,__LINE__, info->device_name,info->tx_count );

	if (!info->tx_enabled ) {
		write_reg(info, CMD, TXRESET);
		write_reg(info, CMD, TXENABLE);
		info->tx_enabled = TRUE;
	}

	if ( info->tx_count ) {

		/* If auto RTS enabled and RTS is inactive, then assert */
		/* RTS and set a flag indicating that the driver should */
		/* negate RTS when the transmission completes. */

		info->drop_rts_on_tx_done = 0;

		if (info->params.mode != MGSL_MODE_ASYNC) {

			if ( info->params.flags & HDLC_FLAG_AUTO_RTS ) {
				get_signals( info );
				if ( !(info->serial_signals & SerialSignal_RTS) ) {
					info->serial_signals |= SerialSignal_RTS;
					set_signals( info );
					info->drop_rts_on_tx_done = 1;
				}
			}

			write_reg(info, TXDMA + DSR, 0); 		/* disable DMA channel */
			write_reg(info, TXDMA + DCMD, SWABORT);	/* reset/init DMA channel */
	
			/* set TX CDA (current descriptor address) */
			write_reg16(info, TXDMA + CDA,
				info->tx_buf_list_ex[0].phys_entry);
	
			/* set TX EDA (last descriptor address) */
			write_reg16(info, TXDMA + EDA,
				info->tx_buf_list_ex[info->last_tx_buf].phys_entry);
	
			/* clear IDLE and UDRN status bit */
			info->ie1_value &= ~(IDLE + UDRN);
			if (info->params.mode != MGSL_MODE_ASYNC)
				info->ie1_value |= UDRN;     		/* HDLC, IRQ on underrun */
			write_reg(info, IE1, info->ie1_value);	/* enable MSCI interrupts */
			write_reg(info, SR1, (unsigned char)(IDLE + UDRN));
	
			write_reg(info, TXDMA + DIR, 0x40);		/* enable Tx DMA interrupts (EOM) */
			write_reg(info, TXDMA + DSR, 0xf2);		/* clear Tx DMA IRQs, enable Tx DMA */
	
			info->tx_timer.expires = jiffies + jiffies_from_ms(5000);
			add_timer(&info->tx_timer);
		}
		else {
			tx_load_fifo(info);
			/* async, enable IRQ on txdata */
			info->ie0_value |= TXRDYE;
			write_reg(info, IE0, info->ie0_value);
		}

		info->tx_active = 1;
	}
}

/* stop the transmitter and DMA
 */
void tx_stop( SLMP_INFO *info )
{
	if (debug_level >= DEBUG_LEVEL_ISR)
		printk("%s(%d):%s tx_stop()\n",
			 __FILE__,__LINE__, info->device_name );

	del_timer(&info->tx_timer);

	write_reg(info, TXDMA + DSR, 0);		/* disable DMA channel */
	write_reg(info, TXDMA + DCMD, SWABORT);	/* reset/init DMA channel */

	write_reg(info, CMD, TXRESET);

	info->ie1_value &= ~(UDRN + IDLE);
	write_reg(info, IE1, info->ie1_value);	/* disable tx status interrupts */
	write_reg(info, SR1, (unsigned char)(IDLE + UDRN));	/* clear pending */

	info->ie0_value &= ~TXRDYE;
	write_reg(info, IE0, info->ie0_value);	/* disable tx data interrupts */

	info->tx_enabled = 0;
	info->tx_active  = 0;
}

/* Fill the transmit FIFO until the FIFO is full or
 * there is no more data to load.
 */
void tx_load_fifo(SLMP_INFO *info)
{
	u8 TwoBytes[2];

	/* do nothing is now tx data available and no XON/XOFF pending */

	if ( !info->tx_count && !info->x_char )
		return;

	/* load the Transmit FIFO until FIFOs full or all data sent */

	while( info->tx_count && (read_reg(info,SR0) & BIT1) ) {

		/* there is more space in the transmit FIFO and */
		/* there is more data in transmit buffer */

		if ( (info->tx_count > 1) && !info->x_char ) {
 			/* write 16-bits */
			TwoBytes[0] = info->tx_buf[info->tx_get++];
			if (info->tx_get >= info->max_frame_size)
				info->tx_get -= info->max_frame_size;
			TwoBytes[1] = info->tx_buf[info->tx_get++];
			if (info->tx_get >= info->max_frame_size)
				info->tx_get -= info->max_frame_size;

			write_reg16(info, TRB, *((u16 *)TwoBytes));

			info->tx_count -= 2;
			info->icount.tx += 2;
		} else {
			/* only 1 byte left to transmit or 1 FIFO slot left */

			if (info->x_char) {
				/* transmit pending high priority char */
				write_reg(info, TRB, info->x_char);
				info->x_char = 0;
			} else {
				write_reg(info, TRB, info->tx_buf[info->tx_get++]);
				if (info->tx_get >= info->max_frame_size)
					info->tx_get -= info->max_frame_size;
				info->tx_count--;
			}
			info->icount.tx++;
		}
	}
}

/* Reset a port to a known state
 */
void reset_port(SLMP_INFO *info)
{
	if (info->sca_base) {

		tx_stop(info);
		rx_stop(info);

		info->serial_signals &= ~(SerialSignal_DTR + SerialSignal_RTS);
		set_signals(info);

		/* disable all port interrupts */
		info->ie0_value = 0;
		info->ie1_value = 0;
		info->ie2_value = 0;
		write_reg(info, IE0, info->ie0_value);
		write_reg(info, IE1, info->ie1_value);
		write_reg(info, IE2, info->ie2_value);

		write_reg(info, CMD, CHRESET);
	}
}

/* Reset all the ports to a known state.
 */
void reset_adapter(SLMP_INFO *info)
{
	int i;

	for ( i=0; i < SCA_MAX_PORTS; ++i) {
		if (info->port_array[i])
			reset_port(info->port_array[i]);
	}
}

/* Program port for asynchronous communications.
 */
void async_mode(SLMP_INFO *info)
{

  	unsigned char RegValue;

	tx_stop(info);
	rx_stop(info);

	/* MD0, Mode Register 0
	 *
	 * 07..05  PRCTL<2..0>, Protocol Mode, 000=async
	 * 04      AUTO, Auto-enable (RTS/CTS/DCD)
	 * 03      Reserved, must be 0
	 * 02      CRCCC, CRC Calculation, 0=disabled
	 * 01..00  STOP<1..0> Stop bits (00=1,10=2)
	 *
	 * 0000 0000
	 */
	RegValue = 0x00;
	if (info->params.stop_bits != 1)
		RegValue |= BIT1;
	write_reg(info, MD0, RegValue);

	/* MD1, Mode Register 1
	 *
	 * 07..06  BRATE<1..0>, bit rate, 00=1/1 01=1/16 10=1/32 11=1/64
	 * 05..04  TXCHR<1..0>, tx char size, 00=8 bits,01=7,10=6,11=5
	 * 03..02  RXCHR<1..0>, rx char size
	 * 01..00  PMPM<1..0>, Parity mode, 00=none 10=even 11=odd
	 *
	 * 0100 0000
	 */
	RegValue = 0x40;
	switch (info->params.data_bits) {
	case 7: RegValue |= BIT4 + BIT2; break;
	case 6: RegValue |= BIT5 + BIT3; break;
	case 5: RegValue |= BIT5 + BIT4 + BIT3 + BIT2; break;
	}
	if (info->params.parity != ASYNC_PARITY_NONE) {
		RegValue |= BIT1;
		if (info->params.parity == ASYNC_PARITY_ODD)
			RegValue |= BIT0;
	}
	write_reg(info, MD1, RegValue);

	/* MD2, Mode Register 2
	 *
	 * 07..02  Reserved, must be 0
	 * 01..00  CNCT<1..0> Channel connection, 0=normal
	 *
	 * 0000 0000
	 */
	RegValue = 0x00;
	write_reg(info, MD2, RegValue);

	/* RXS, Receive clock source
	 *
	 * 07      Reserved, must be 0
	 * 06..04  RXCS<2..0>, clock source, 000=RxC Pin, 100=BRG, 110=DPLL
	 * 03..00  RXBR<3..0>, rate divisor, 0000=1
	 */
	RegValue=BIT6;
	write_reg(info, RXS, RegValue);

	/* TXS, Transmit clock source
	 *
	 * 07      Reserved, must be 0
	 * 06..04  RXCS<2..0>, clock source, 000=TxC Pin, 100=BRG, 110=Receive Clock
	 * 03..00  RXBR<3..0>, rate divisor, 0000=1
	 */
	RegValue=BIT6;
	write_reg(info, TXS, RegValue);

	/* Control Register
	 *
	 * 6,4,2,0  CLKSEL<3..0>, 0 = TcCLK in, 1 = Auxclk out
	 */
	info->port_array[0]->ctrlreg_value |= (BIT0 << (info->port_num * 2));
	write_control_reg(info);

	tx_set_idle(info);

	/* RRC Receive Ready Control 0
	 *
	 * 07..05  Reserved, must be 0
	 * 04..00  RRC<4..0> Rx FIFO trigger active 0x00 = 1 byte
	 */
	write_reg(info, TRC0, 0x00);

	/* TRC0 Transmit Ready Control 0
	 *
	 * 07..05  Reserved, must be 0
	 * 04..00  TRC<4..0> Tx FIFO trigger active 0x10 = 16 bytes
	 */
	write_reg(info, TRC0, 0x10);

	/* TRC1 Transmit Ready Control 1
	 *
	 * 07..05  Reserved, must be 0
	 * 04..00  TRC<4..0> Tx FIFO trigger inactive 0x1e = 31 bytes (full-1)
	 */
	write_reg(info, TRC1, 0x1e);

	/* CTL, MSCI control register
	 *
	 * 07..06  Reserved, set to 0
	 * 05      UDRNC, underrun control, 0=abort 1=CRC+flag (HDLC/BSC)
	 * 04      IDLC, idle control, 0=mark 1=idle register
	 * 03      BRK, break, 0=off 1 =on (async)
	 * 02      SYNCLD, sync char load enable (BSC) 1=enabled
	 * 01      GOP, go active on poll (LOOP mode) 1=enabled
	 * 00      RTS, RTS output control, 0=active 1=inactive
	 *
	 * 0001 0001
	 */
	RegValue = 0x10;
	if (!(info->serial_signals & SerialSignal_RTS))
		RegValue |= 0x01;
	write_reg(info, CTL, RegValue);

	/* enable status interrupts */
	info->ie0_value |= TXINTE + RXINTE;
	write_reg(info, IE0, info->ie0_value);

	/* enable break detect interrupt */
	info->ie1_value = BRKD;
	write_reg(info, IE1, info->ie1_value);

	/* enable rx overrun interrupt */
	info->ie2_value = OVRN;
	write_reg(info, IE2, info->ie2_value);

	set_rate( info, info->params.data_rate * 16 );

	if (info->params.loopback)
		enable_loopback(info,1);
}

/* Program the SCA for HDLC communications.
 */
void hdlc_mode(SLMP_INFO *info)
{
	unsigned char RegValue;
	u32 DpllDivisor;

	// Can't use DPLL because SCA outputs recovered clock on RxC when
	// DPLL mode selected. This causes output contention with RxC receiver.
	// Use of DPLL would require external hardware to disable RxC receiver
	// when DPLL mode selected.
	info->params.flags &= ~(HDLC_FLAG_TXC_DPLL + HDLC_FLAG_RXC_DPLL);

	/* disable DMA interrupts */
	write_reg(info, TXDMA + DIR, 0);
	write_reg(info, RXDMA + DIR, 0);

	/* MD0, Mode Register 0
	 *
	 * 07..05  PRCTL<2..0>, Protocol Mode, 100=HDLC
	 * 04      AUTO, Auto-enable (RTS/CTS/DCD)
	 * 03      Reserved, must be 0
	 * 02      CRCCC, CRC Calculation, 1=enabled
	 * 01      CRC1, CRC selection, 0=CRC-16,1=CRC-CCITT-16
	 * 00      CRC0, CRC initial value, 1 = all 1s
	 *
	 * 1000 0001
	 */
	RegValue = 0x81;
	if (info->params.flags & HDLC_FLAG_AUTO_CTS)
		RegValue |= BIT4;
	if (info->params.flags & HDLC_FLAG_AUTO_DCD)
		RegValue |= BIT4;
	if (info->params.crc_type == HDLC_CRC_16_CCITT)
		RegValue |= BIT2 + BIT1;
	write_reg(info, MD0, RegValue);

	/* MD1, Mode Register 1
	 *
	 * 07..06  ADDRS<1..0>, Address detect, 00=no addr check
	 * 05..04  TXCHR<1..0>, tx char size, 00=8 bits
	 * 03..02  RXCHR<1..0>, rx char size, 00=8 bits
	 * 01..00  PMPM<1..0>, Parity mode, 00=no parity
	 *
	 * 0000 0000
	 */
	RegValue = 0x00;
	write_reg(info, MD1, RegValue);

	/* MD2, Mode Register 2
	 *
	 * 07      NRZFM, 0=NRZ, 1=FM
	 * 06..05  CODE<1..0> Encoding, 00=NRZ
	 * 04..03  DRATE<1..0> DPLL Divisor, 00=8
	 * 02      Reserved, must be 0
	 * 01..00  CNCT<1..0> Channel connection, 0=normal
	 *
	 * 0000 0000
	 */
	RegValue = 0x00;
	switch(info->params.encoding) {
	case HDLC_ENCODING_NRZI:	  RegValue |= BIT5; break;
	case HDLC_ENCODING_BIPHASE_MARK:  RegValue |= BIT7 + BIT5; break; /* aka FM1 */
	case HDLC_ENCODING_BIPHASE_SPACE: RegValue |= BIT7 + BIT6; break; /* aka FM0 */
	case HDLC_ENCODING_BIPHASE_LEVEL: RegValue |= BIT7; break; 	/* aka Manchester */
	}
	if ( info->params.flags & HDLC_FLAG_DPLL_DIV16 ) {
		DpllDivisor = 16;
		RegValue |= BIT3;
	} else if ( info->params.flags & HDLC_FLAG_DPLL_DIV8 ) {
		DpllDivisor = 8;
	} else {
		DpllDivisor = 32;
		RegValue |= BIT4;
	}
	write_reg(info, MD2, RegValue);


	/* RXS, Receive clock source
	 *
	 * 07      Reserved, must be 0
	 * 06..04  RXCS<2..0>, clock source, 000=RxC Pin, 100=BRG, 110=DPLL
	 * 03..00  RXBR<3..0>, rate divisor, 0000=1
	 */
	RegValue=0;
	if (info->params.flags & HDLC_FLAG_RXC_BRG)
		RegValue |= BIT6;
	if (info->params.flags & HDLC_FLAG_RXC_DPLL)
		RegValue |= BIT6 + BIT5;
	write_reg(info, RXS, RegValue);

	/* TXS, Transmit clock source
	 *
	 * 07      Reserved, must be 0
	 * 06..04  RXCS<2..0>, clock source, 000=TxC Pin, 100=BRG, 110=Receive Clock
	 * 03..00  RXBR<3..0>, rate divisor, 0000=1
	 */
	RegValue=0;
	if (info->params.flags & HDLC_FLAG_TXC_BRG)
		RegValue |= BIT6;
	if (info->params.flags & HDLC_FLAG_TXC_DPLL)
		RegValue |= BIT6 + BIT5;
	write_reg(info, TXS, RegValue);

	if (info->params.flags & HDLC_FLAG_RXC_DPLL)
		set_rate(info, info->params.clock_speed * DpllDivisor);
	else
		set_rate(info, info->params.clock_speed);

	/* GPDATA (General Purpose I/O Data Register)
	 *
	 * 6,4,2,0  CLKSEL<3..0>, 0 = TcCLK in, 1 = Auxclk out
	 */
	if (info->params.flags & HDLC_FLAG_TXC_BRG)
		info->port_array[0]->ctrlreg_value |= (BIT0 << (info->port_num * 2));
	else
		info->port_array[0]->ctrlreg_value &= ~(BIT0 << (info->port_num * 2));
	write_control_reg(info);

	/* RRC Receive Ready Control 0
	 *
	 * 07..05  Reserved, must be 0
	 * 04..00  RRC<4..0> Rx FIFO trigger active
	 */
	write_reg(info, RRC, rx_active_fifo_level);

	/* TRC0 Transmit Ready Control 0
	 *
	 * 07..05  Reserved, must be 0
	 * 04..00  TRC<4..0> Tx FIFO trigger active
	 */
	write_reg(info, TRC0, tx_active_fifo_level);

	/* TRC1 Transmit Ready Control 1
	 *
	 * 07..05  Reserved, must be 0
	 * 04..00  TRC<4..0> Tx FIFO trigger inactive 0x1f = 32 bytes (full)
	 */
	write_reg(info, TRC1, (unsigned char)(tx_negate_fifo_level - 1));

	/* DMR, DMA Mode Register
	 *
	 * 07..05  Reserved, must be 0
	 * 04      TMOD, Transfer Mode: 1=chained-block
	 * 03      Reserved, must be 0
	 * 02      NF, Number of Frames: 1=multi-frame
	 * 01      CNTE, Frame End IRQ Counter enable: 0=disabled
	 * 00      Reserved, must be 0
	 *
	 * 0001 0100
	 */
	write_reg(info, TXDMA + DMR, 0x14);
	write_reg(info, RXDMA + DMR, 0x14);

	/* Set chain pointer base (upper 8 bits of 24 bit addr) */
	write_reg(info, RXDMA + CPB,
		(unsigned char)(info->buffer_list_phys >> 16));

	/* Set chain pointer base (upper 8 bits of 24 bit addr) */
	write_reg(info, TXDMA + CPB,
		(unsigned char)(info->buffer_list_phys >> 16));

	/* enable status interrupts. other code enables/disables
	 * the individual sources for these two interrupt classes.
	 */
	info->ie0_value |= TXINTE + RXINTE;
	write_reg(info, IE0, info->ie0_value);

	/* CTL, MSCI control register
	 *
	 * 07..06  Reserved, set to 0
	 * 05      UDRNC, underrun control, 0=abort 1=CRC+flag (HDLC/BSC)
	 * 04      IDLC, idle control, 0=mark 1=idle register
	 * 03      BRK, break, 0=off 1 =on (async)
	 * 02      SYNCLD, sync char load enable (BSC) 1=enabled
	 * 01      GOP, go active on poll (LOOP mode) 1=enabled
	 * 00      RTS, RTS output control, 0=active 1=inactive
	 *
	 * 0001 0001
	 */
	RegValue = 0x10;
	if (!(info->serial_signals & SerialSignal_RTS))
		RegValue |= 0x01;
	write_reg(info, CTL, RegValue);

	/* preamble not supported ! */

	tx_set_idle(info);
	tx_stop(info);
	rx_stop(info);

	set_rate(info, info->params.clock_speed);

	if (info->params.loopback)
		enable_loopback(info,1);
}

/* Set the transmit HDLC idle mode
 */
void tx_set_idle(SLMP_INFO *info)
{
	unsigned char RegValue = 0xff;

	/* Map API idle mode to SCA register bits */
	switch(info->idle_mode) {
	case HDLC_TXIDLE_FLAGS:			RegValue = 0x7e; break;
	case HDLC_TXIDLE_ALT_ZEROS_ONES:	RegValue = 0xaa; break;
	case HDLC_TXIDLE_ZEROS:			RegValue = 0x00; break;
	case HDLC_TXIDLE_ONES:			RegValue = 0xff; break;
	case HDLC_TXIDLE_ALT_MARK_SPACE:	RegValue = 0xaa; break;
	case HDLC_TXIDLE_SPACE:			RegValue = 0x00; break;
	case HDLC_TXIDLE_MARK:			RegValue = 0xff; break;
	}

	write_reg(info, IDL, RegValue);
}

/* Query the adapter for the state of the V24 status (input) signals.
 */
void get_signals(SLMP_INFO *info)
{
	u16 status = read_reg(info, SR3);
	u16 gpstatus = read_status_reg(info);
	u16 testbit;

	/* clear all serial signals except DTR and RTS */
	info->serial_signals &= SerialSignal_DTR + SerialSignal_RTS;

	/* set serial signal bits to reflect MISR */

	if (!(status & BIT3))
		info->serial_signals |= SerialSignal_CTS;

	if ( !(status & BIT2))
		info->serial_signals |= SerialSignal_DCD;

	testbit = BIT1 << (info->port_num * 2); // Port 0..3 RI is GPDATA<1,3,5,7>
	if (!(gpstatus & testbit))
		info->serial_signals |= SerialSignal_RI;

	testbit = BIT0 << (info->port_num * 2); // Port 0..3 DSR is GPDATA<0,2,4,6>
	if (!(gpstatus & testbit))
		info->serial_signals |= SerialSignal_DSR;
}

/* Set the state of DTR and RTS based on contents of
 * serial_signals member of device context.
 */
void set_signals(SLMP_INFO *info)
{
	unsigned char RegValue;
	u16 EnableBit;

	RegValue = read_reg(info, CTL);
	if (info->serial_signals & SerialSignal_RTS)
		RegValue &= ~BIT0;
	else
		RegValue |= BIT0;
	write_reg(info, CTL, RegValue);

	// Port 0..3 DTR is ctrl reg <1,3,5,7>
	EnableBit = BIT1 << (info->port_num*2);
	if (info->serial_signals & SerialSignal_DTR)
		info->port_array[0]->ctrlreg_value &= ~EnableBit;
	else
		info->port_array[0]->ctrlreg_value |= EnableBit;
	write_control_reg(info);
}

/*******************/
/* DMA Buffer Code */
/*******************/

/* Set the count for all receive buffers to SCABUFSIZE
 * and set the current buffer to the first buffer. This effectively
 * makes all buffers free and discards any data in buffers.
 */
void rx_reset_buffers(SLMP_INFO *info)
{
	rx_free_frame_buffers(info, 0, info->rx_buf_count - 1);
}

/* Free the buffers used by a received frame
 *
 * info   pointer to device instance data
 * first  index of 1st receive buffer of frame
 * last   index of last receive buffer of frame
 */
void rx_free_frame_buffers(SLMP_INFO *info, unsigned int first, unsigned int last)
{
	int done = 0;

	while(!done) {
	        /* reset current buffer for reuse */
		info->rx_buf_list[first].status = 0xff;

	        if (first == last) {
	                done = 1;
	                /* set new last rx descriptor address */
			write_reg16(info, RXDMA + EDA, info->rx_buf_list_ex[first].phys_entry);
	        }

	        first++;
		if (first == info->rx_buf_count)
			first = 0;
	}

	/* set current buffer to next buffer after last buffer of frame */
	info->current_rx_buf = first;
}

/* Return a received frame from the receive DMA buffers.
 * Only frames received without errors are returned.
 *
 * Return Value:	1 if frame returned, otherwise 0
 */
int rx_get_frame(SLMP_INFO *info)
{
	unsigned int StartIndex, EndIndex;	/* index of 1st and last buffers of Rx frame */
	unsigned short status;
	unsigned int framesize = 0;
	int ReturnCode = 0;
	unsigned long flags;
	struct tty_struct *tty = info->tty;
	unsigned char addr_field = 0xff;
   	SCADESC *desc;
	SCADESC_EX *desc_ex;

CheckAgain:
	/* assume no frame returned, set zero length */
	framesize = 0;
	addr_field = 0xff;

	/*
	 * current_rx_buf points to the 1st buffer of the next available
	 * receive frame. To find the last buffer of the frame look for
	 * a non-zero status field in the buffer entries. (The status
	 * field is set by the 16C32 after completing a receive frame.
	 */
	StartIndex = EndIndex = info->current_rx_buf;

	for ( ;; ) {
		desc = &info->rx_buf_list[EndIndex];
		desc_ex = &info->rx_buf_list_ex[EndIndex];

		if (desc->status == 0xff)
			goto Cleanup;	/* current desc still in use, no frames available */

		if (framesize == 0 && info->params.addr_filter != 0xff)
			addr_field = desc_ex->virt_addr[0];

		framesize += desc->length;

		/* Status != 0 means last buffer of frame */
		if (desc->status)
			break;

		EndIndex++;
		if (EndIndex == info->rx_buf_count)
			EndIndex = 0;

		if (EndIndex == info->current_rx_buf) {
			/* all buffers have been 'used' but none mark	   */
			/* the end of a frame. Reset buffers and receiver. */
			if ( info->rx_enabled ){
				spin_lock_irqsave(&info->lock,flags);
				rx_start(info);
				spin_unlock_irqrestore(&info->lock,flags);
			}
			goto Cleanup;
		}

	}

	/* check status of receive frame */

	/* frame status is byte stored after frame data
	 *
	 * 7 EOM (end of msg), 1 = last buffer of frame
	 * 6 Short Frame, 1 = short frame
	 * 5 Abort, 1 = frame aborted
	 * 4 Residue, 1 = last byte is partial
	 * 3 Overrun, 1 = overrun occurred during frame reception
	 * 2 CRC,     1 = CRC error detected
	 *
	 */
	status = desc->status;

	/* ignore CRC bit if not using CRC (bit is undefined) */
	/* Note:CRC is not save to data buffer */
	if (info->params.crc_type == HDLC_CRC_NONE)
		status &= ~BIT2;

	if (framesize == 0 ||
		 (addr_field != 0xff && addr_field != info->params.addr_filter)) {
		/* discard 0 byte frames, this seems to occur sometime
		 * when remote is idling flags.
		 */
		rx_free_frame_buffers(info, StartIndex, EndIndex);
		goto CheckAgain;
	}

	if (framesize < 2)
		status |= BIT6;

	if (status & (BIT6+BIT5+BIT3+BIT2)) {
		/* received frame has errors,
		 * update counts and mark frame size as 0
		 */
		if (status & BIT6)
			info->icount.rxshort++;
		else if (status & BIT5)
			info->icount.rxabort++;
		else if (status & BIT3)
			info->icount.rxover++;
		else
			info->icount.rxcrc++;

		framesize = 0;

#ifdef CONFIG_SYNCLINK_SYNCPPP
		info->netstats.rx_errors++;
		info->netstats.rx_frame_errors++;
#endif
	}

	if ( debug_level >= DEBUG_LEVEL_BH )
		printk("%s(%d):%s rx_get_frame() status=%04X size=%d\n",
			__FILE__,__LINE__,info->device_name,status,framesize);

	if ( debug_level >= DEBUG_LEVEL_DATA )
		trace_block(info,info->rx_buf_list_ex[StartIndex].virt_addr,
			MIN(framesize,SCABUFSIZE),0);

	if (framesize) {
		if (framesize > info->max_frame_size)
			info->icount.rxlong++;
		else {
			/* copy dma buffer(s) to contiguous intermediate buffer */
			int copy_count = framesize;
			int index = StartIndex;
			unsigned char *ptmp = info->tmp_rx_buf;
			info->tmp_rx_buf_count = framesize;

			info->icount.rxok++;

			while(copy_count) {
				int partial_count = MIN(copy_count,SCABUFSIZE);
				memcpy( ptmp,
					info->rx_buf_list_ex[index].virt_addr,
					partial_count );
				ptmp += partial_count;
				copy_count -= partial_count;

				if ( ++index == info->rx_buf_count )
					index = 0;
			}

#ifdef CONFIG_SYNCLINK_SYNCPPP
			if (info->netcount) {
				/* pass frame to syncppp device */
				sppp_rx_done(info,info->tmp_rx_buf,framesize);
			}
			else
#endif
			{
				if ( tty && tty->ldisc.receive_buf ) {
					/* Call the line discipline receive callback directly. */
					tty->ldisc.receive_buf(tty,
						info->tmp_rx_buf,
						info->flag_buf,
						framesize);
				}
			}
		}
	}
	/* Free the buffers used by this frame. */
	rx_free_frame_buffers( info, StartIndex, EndIndex );

	ReturnCode = 1;

Cleanup:
	if ( info->rx_enabled && info->rx_overflow ) {
		/* Receiver is enabled, but needs to restarted due to
		 * rx buffer overflow. If buffers are empty, restart receiver.
		 */
		if (info->rx_buf_list[EndIndex].status == 0xff) {
			spin_lock_irqsave(&info->lock,flags);
			rx_start(info);
			spin_unlock_irqrestore(&info->lock,flags);
		}
	}

	return ReturnCode;
}

/* load the transmit DMA buffer with data
 */
void tx_load_dma_buffer(SLMP_INFO *info, const char *buf, unsigned int count)
{
	unsigned short copy_count;
	unsigned int i = 0;
	SCADESC *desc;
	SCADESC_EX *desc_ex;

	if ( debug_level >= DEBUG_LEVEL_DATA )
		trace_block(info,buf, MIN(count,SCABUFSIZE), 1);

	/* Copy source buffer to one or more DMA buffers, starting with
	 * the first transmit dma buffer.
	 */
	for(i=0;;)
	{
		copy_count = MIN(count,SCABUFSIZE);

		desc = &info->tx_buf_list[i];
		desc_ex = &info->tx_buf_list_ex[i];

		load_pci_memory(info, desc_ex->virt_addr,buf,copy_count);

		desc->length = copy_count;
		desc->status = 0;

		buf += copy_count;
		count -= copy_count;

		if (!count)
			break;

		i++;
		if (i >= info->tx_buf_count)
			i = 0;
	}

	info->tx_buf_list[i].status = 0x81;	/* set EOM and EOT status */
	info->last_tx_buf = ++i;
}

int register_test(SLMP_INFO *info)
{
	static unsigned char testval[] = {0x00, 0xff, 0xaa, 0x55, 0x69, 0x96};
	static unsigned int count = sizeof(testval)/sizeof(unsigned char);
	unsigned int i;
	int rc = TRUE;
	unsigned long flags;

	spin_lock_irqsave(&info->lock,flags);
	reset_port(info);

	/* assume failure */
	info->init_error = DiagStatus_AddressFailure;

	/* Write bit patterns to various registers but do it out of */
	/* sync, then read back and verify values. */

	for (i = 0 ; i < count ; i++) {
		write_reg(info, TMC, testval[i]);
		write_reg(info, IDL, testval[(i+1)%count]);
		write_reg(info, SA0, testval[(i+2)%count]);
		write_reg(info, SA1, testval[(i+3)%count]);

		if ( (read_reg(info, TMC) != testval[i]) ||
			  (read_reg(info, IDL) != testval[(i+1)%count]) ||
			  (read_reg(info, SA0) != testval[(i+2)%count]) ||
			  (read_reg(info, SA1) != testval[(i+3)%count]) )
		{
			rc = FALSE;
			break;
		}
	}

	reset_port(info);
	spin_unlock_irqrestore(&info->lock,flags);

	return rc;
}

int irq_test(SLMP_INFO *info)
{
	unsigned long timeout;
	unsigned long flags;

	unsigned char timer = (info->port_num & 1) ? TIMER2 : TIMER0;

	spin_lock_irqsave(&info->lock,flags);
	reset_port(info);

	/* assume failure */
	info->init_error = DiagStatus_IrqFailure;
	info->irq_occurred = FALSE;

	/* setup timer0 on SCA0 to interrupt */

	/* IER2<7..4> = timer<3..0> interrupt enables (1=enabled) */
	write_reg(info, IER2, (unsigned char)((info->port_num & 1) ? BIT6 : BIT4));

	write_reg(info, (unsigned char)(timer + TEPR), 0);	/* timer expand prescale */
	write_reg16(info, (unsigned char)(timer + TCONR), 1);	/* timer constant */


	/* TMCS, Timer Control/Status Register
	 *
	 * 07      CMF, Compare match flag (read only) 1=match
	 * 06      ECMI, CMF Interrupt Enable: 1=enabled
	 * 05      Reserved, must be 0
	 * 04      TME, Timer Enable
	 * 03..00  Reserved, must be 0
	 *
	 * 0101 0000
	 */
	write_reg(info, (unsigned char)(timer + TMCS), 0x50);

	spin_unlock_irqrestore(&info->lock,flags);

	timeout=100;
	while( timeout-- && !info->irq_occurred ) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(jiffies_from_ms(10));
	}

	spin_lock_irqsave(&info->lock,flags);
	reset_port(info);
	spin_unlock_irqrestore(&info->lock,flags);

	return info->irq_occurred;
}

/* initialize individual SCA device (2 ports)
 */
int sca_init(SLMP_INFO *info)
{
	/* set wait controller to single mem partition (low), no wait states */
	write_reg(info, PABR0, 0);	/* wait controller addr boundary 0 */
	write_reg(info, PABR1, 0);	/* wait controller addr boundary 1 */
	write_reg(info, WCRL, 0);	/* wait controller low range */
	write_reg(info, WCRM, 0);	/* wait controller mid range */
	write_reg(info, WCRH, 0);	/* wait controller high range */

	/* DPCR, DMA Priority Control
	 *
	 * 07..05  Not used, must be 0
	 * 04      BRC, bus release condition: 0=all transfers complete
	 * 03      CCC, channel change condition: 0=every cycle
	 * 02..00  PR<2..0>, priority 100=round robin
	 *
	 * 00000100 = 0x04
	 */
	write_reg(info, DPCR, dma_priority);

	/* DMA Master Enable, BIT7: 1=enable all channels */
	write_reg(info, DMER, 0x80);

	/* enable all interrupt classes */
	write_reg(info, IER0, 0xff);	/* TxRDY,RxRDY,TxINT,RxINT (ports 0-1) */
	write_reg(info, IER1, 0xff);	/* DMIB,DMIA (channels 0-3) */
	write_reg(info, IER2, 0xf0);	/* TIRQ (timers 0-3) */

	/* ITCR, interrupt control register
	 * 07      IPC, interrupt priority, 0=MSCI->DMA
	 * 06..05  IAK<1..0>, Acknowledge cycle, 00=non-ack cycle
	 * 04      VOS, Vector Output, 0=unmodified vector
	 * 03..00  Reserved, must be 0
	 */
	write_reg(info, ITCR, 0);

	return TRUE;
}

/* initialize adapter hardware
 */
int init_adapter(SLMP_INFO *info)
{
	int i;

	/* Set BIT30 of Local Control Reg 0x50 to reset SCA */
	volatile u32 *MiscCtrl = (u32 *)(info->lcr_base + 0x50);
	u32 readval;

	info->misc_ctrl_value |= BIT30;
	*MiscCtrl = info->misc_ctrl_value;

	/*
	 * Force at least 170ns delay before clearing
	 * reset bit. Each read from LCR takes at least
	 * 30ns so 10 times for 300ns to be safe.
	 */
	for(i=0;i<10;i++)
		readval = *MiscCtrl;

	info->misc_ctrl_value &= ~BIT30;
	*MiscCtrl = info->misc_ctrl_value;

	/* init control reg (all DTRs off, all clksel=input) */
	info->ctrlreg_value = 0xaa;
	write_control_reg(info);

	{
		volatile u32 *LCR1BRDR = (u32 *)(info->lcr_base + 0x2c);
		lcr1_brdr_value &= ~(BIT5 + BIT4 + BIT3);

		switch(read_ahead_count)
		{
		case 16:
			lcr1_brdr_value |= BIT5 + BIT4 + BIT3;
			break;
		case 8:
			lcr1_brdr_value |= BIT5 + BIT4;
			break;
		case 4:
			lcr1_brdr_value |= BIT5 + BIT3;
			break;
		case 0:
			lcr1_brdr_value |= BIT5;
			break;
		}

		*LCR1BRDR = lcr1_brdr_value;
		*MiscCtrl = misc_ctrl_value;
	}

	sca_init(info->port_array[0]);
	sca_init(info->port_array[2]);

	return TRUE;
}

/* Loopback an HDLC frame to test the hardware
 * interrupt and DMA functions.
 */
int loopback_test(SLMP_INFO *info)
{
#define TESTFRAMESIZE 20

	unsigned long timeout;
	u16 count = TESTFRAMESIZE;
	unsigned char buf[TESTFRAMESIZE];
	int rc = FALSE;
	unsigned long flags;

	struct tty_struct *oldtty = info->tty;
	u32 speed = info->params.clock_speed;

	info->params.clock_speed = 3686400;
	info->tty = 0;

	/* assume failure */
	info->init_error = DiagStatus_DmaFailure;

	/* build and send transmit frame */
	for (count = 0; count < TESTFRAMESIZE;++count)
		buf[count] = (unsigned char)count;

	memset(info->tmp_rx_buf,0,TESTFRAMESIZE);

	/* program hardware for HDLC and enabled receiver */
	spin_lock_irqsave(&info->lock,flags);
	hdlc_mode(info);
	enable_loopback(info,1);
       	rx_start(info);
	info->tx_count = count;
	tx_load_dma_buffer(info,buf,count);
	tx_start(info);
	spin_unlock_irqrestore(&info->lock,flags);

	/* wait for receive complete */
	/* Set a timeout for waiting for interrupt. */
	for ( timeout = 100; timeout; --timeout ) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(jiffies_from_ms(10));

		if (rx_get_frame(info)) {
			rc = TRUE;
			break;
		}
	}

	/* verify received frame length and contents */
	if (rc == TRUE &&
		( info->tmp_rx_buf_count != count ||
		  memcmp(buf, info->tmp_rx_buf,count))) {
		rc = FALSE;
	}

	spin_lock_irqsave(&info->lock,flags);
	reset_adapter(info);
	spin_unlock_irqrestore(&info->lock,flags);

	info->params.clock_speed = speed;
	info->tty = oldtty;

	return rc;
}

/* Perform diagnostics on hardware
 */
int adapter_test( SLMP_INFO *info )
{
	unsigned long flags;
	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):Testing device %s\n",
			__FILE__,__LINE__,info->device_name );

	spin_lock_irqsave(&info->lock,flags);
	init_adapter(info);
	spin_unlock_irqrestore(&info->lock,flags);

	info->port_array[0]->port_count = 0;

	if ( register_test(info->port_array[0]) &&
		register_test(info->port_array[1])) {

		info->port_array[0]->port_count = 2;

		if ( register_test(info->port_array[2]) &&
			register_test(info->port_array[3]) )
			info->port_array[0]->port_count += 2;
	}
	else {
		printk( "%s(%d):Register test failure for device %s Addr=%08lX\n",
			__FILE__,__LINE__,info->device_name, (unsigned long)(info->phys_sca_base));
		return -ENODEV;
	}

	if ( !irq_test(info->port_array[0]) ||
		!irq_test(info->port_array[1]) ||
		 (info->port_count == 4 && !irq_test(info->port_array[2])) ||
		 (info->port_count == 4 && !irq_test(info->port_array[3]))) {
		printk( "%s(%d):Interrupt test failure for device %s IRQ=%d\n",
			__FILE__,__LINE__,info->device_name, (unsigned short)(info->irq_level) );
		return -ENODEV;
	}

	if (!loopback_test(info->port_array[0]) ||
		!loopback_test(info->port_array[1]) ||
		 (info->port_count == 4 && !loopback_test(info->port_array[2])) ||
		 (info->port_count == 4 && !loopback_test(info->port_array[3]))) {
		printk( "%s(%d):DMA test failure for device %s\n",
			__FILE__,__LINE__,info->device_name);
		return -ENODEV;
	}

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):device %s passed diagnostics\n",
			__FILE__,__LINE__,info->device_name );

	info->port_array[0]->init_error = 0;
	info->port_array[1]->init_error = 0;
	if ( info->port_count > 2 ) {
		info->port_array[2]->init_error = 0;
		info->port_array[3]->init_error = 0;
	}

	return 0;
}

/* Test the shared memory on a PCI adapter.
 */
int memory_test(SLMP_INFO *info)
{
	static unsigned long testval[] = { 0x0, 0x55555555, 0xaaaaaaaa,
		0x66666666, 0x99999999, 0xffffffff, 0x12345678 };
	unsigned long count = sizeof(testval)/sizeof(unsigned long);
	unsigned long i;
	unsigned long limit = SCA_MEM_SIZE/sizeof(unsigned long);
	unsigned long * addr = (unsigned long *)info->memory_base;

	/* Test data lines with test pattern at one location. */

	for ( i = 0 ; i < count ; i++ ) {
		*addr = testval[i];
		if ( *addr != testval[i] )
			return FALSE;
	}

	/* Test address lines with incrementing pattern over */
	/* entire address range. */

	for ( i = 0 ; i < limit ; i++ ) {
		*addr = i * 4;
		addr++;
	}

	addr = (unsigned long *)info->memory_base;

	for ( i = 0 ; i < limit ; i++ ) {
		if ( *addr != i * 4 )
			return FALSE;
		addr++;
	}

	memset( info->memory_base, 0, SCA_MEM_SIZE );
	return TRUE;
}

/* Load data into PCI adapter shared memory.
 *
 * The PCI9050 releases control of the local bus
 * after completing the current read or write operation.
 *
 * While the PCI9050 write FIFO not empty, the
 * PCI9050 treats all of the writes as a single transaction
 * and does not release the bus. This causes DMA latency problems
 * at high speeds when copying large data blocks to the shared memory.
 *
 * This function breaks a write into multiple transations by
 * interleaving a read which flushes the write FIFO and 'completes'
 * the write transation. This allows any pending DMA request to gain control
 * of the local bus in a timely fasion.
 */
void load_pci_memory(SLMP_INFO *info, char* dest, const char* src, unsigned short count)
{
	/* A load interval of 16 allows for 4 32-bit writes at */
	/* 136ns each for a maximum latency of 542ns on the local bus.*/

	unsigned short interval = count / sca_pci_load_interval;
	unsigned short i;

	for ( i = 0 ; i < interval ; i++ )
	{
		memcpy(dest, src, sca_pci_load_interval);
		read_status_reg(info);
		dest += sca_pci_load_interval;
		src += sca_pci_load_interval;
	}

	memcpy(dest, src, count % sca_pci_load_interval);
}

void trace_block(SLMP_INFO *info,const char* data, int count, int xmit)
{
	int i;
	int linecount;
	if (xmit)
		printk("%s tx data:\n",info->device_name);
	else
		printk("%s rx data:\n",info->device_name);

	while(count) {
		if (count > 16)
			linecount = 16;
		else
			linecount = count;

		for(i=0;i<linecount;i++)
			printk("%02X ",(unsigned char)data[i]);
		for(;i<17;i++)
			printk("   ");
		for(i=0;i<linecount;i++) {
			if (data[i]>=040 && data[i]<=0176)
				printk("%c",data[i]);
			else
				printk(".");
		}
		printk("\n");

		data  += linecount;
		count -= linecount;
	}
}	/* end of trace_block() */

/* called when HDLC frame times out
 * update stats and do tx completion processing
 */
void tx_timeout(unsigned long context)
{
	SLMP_INFO *info = (SLMP_INFO*)context;
	unsigned long flags;

	if ( debug_level >= DEBUG_LEVEL_INFO )
		printk( "%s(%d):%s tx_timeout()\n",
			__FILE__,__LINE__,info->device_name);
	if(info->tx_active && info->params.mode == MGSL_MODE_HDLC) {
		info->icount.txtimeout++;
	}
	spin_lock_irqsave(&info->lock,flags);
	info->tx_active = 0;
	info->tx_count = info->tx_put = info->tx_get = 0;

	spin_unlock_irqrestore(&info->lock,flags);

#ifdef CONFIG_SYNCLINK_SYNCPPP
	if (info->netcount)
		sppp_tx_done(info);
	else
#endif
		bh_transmit(info);
}

/* called to periodically check the DSR/RI modem signal input status
 */
void status_timeout(unsigned long context)
{
	u16 status = 0;
	SLMP_INFO *info = (SLMP_INFO*)context;
	unsigned long flags;
	unsigned char delta;


	spin_lock_irqsave(&info->lock,flags);
	get_signals(info);
	spin_unlock_irqrestore(&info->lock,flags);

	/* check for DSR/RI state change */

	delta = info->old_signals ^ info->serial_signals;
	info->old_signals = info->serial_signals;

	if (delta & SerialSignal_DSR)
		status |= MISCSTATUS_DSR_LATCHED|(info->serial_signals&SerialSignal_DSR);

	if (delta & SerialSignal_RI)
		status |= MISCSTATUS_RI_LATCHED|(info->serial_signals&SerialSignal_RI);

	if (delta & SerialSignal_DCD)
		status |= MISCSTATUS_DCD_LATCHED|(info->serial_signals&SerialSignal_DCD);

	if (delta & SerialSignal_CTS)
		status |= MISCSTATUS_CTS_LATCHED|(info->serial_signals&SerialSignal_CTS);

	if (status)
		isr_io_pin(info,status);

	info->status_timer.data = (unsigned long)info;
	info->status_timer.function = status_timeout;
	info->status_timer.expires = jiffies + jiffies_from_ms(10);
	add_timer(&info->status_timer);
}


/* Register Access Routines -
 * All registers are memory mapped
 */
#define CALC_REGADDR() \
	unsigned char * RegAddr = (unsigned char*)(info->sca_base + Addr); \
	if (info->port_num > 1) \
		RegAddr += 256;	    		/* port 0-1 SCA0, 2-3 SCA1 */ \
	if ( info->port_num & 1) { \
		if (Addr > 0x7f) \
			RegAddr += 0x40;	/* DMA access */ \
		else if (Addr > 0x1f && Addr < 0x60) \
			RegAddr += 0x20;	/* MSCI access */ \
	}


unsigned char read_reg(SLMP_INFO * info, unsigned char Addr)
{
	CALC_REGADDR();
	return *RegAddr;
}
void write_reg(SLMP_INFO * info, unsigned char Addr, unsigned char Value)
{
	CALC_REGADDR();
	*RegAddr = Value;
}

u16 read_reg16(SLMP_INFO * info, unsigned char Addr)
{
	CALC_REGADDR();
	return *((u16 *)RegAddr);
}

void write_reg16(SLMP_INFO * info, unsigned char Addr, u16 Value)
{
	CALC_REGADDR();
	*((u16 *)RegAddr) = Value;
}

unsigned char read_status_reg(SLMP_INFO * info)
{
	unsigned char *RegAddr = (unsigned char *)info->statctrl_base;
	return *RegAddr;
}

void write_control_reg(SLMP_INFO * info)
{
	unsigned char *RegAddr = (unsigned char *)info->statctrl_base;
	*RegAddr = info->port_array[0]->ctrlreg_value;
}


static int __devinit synclinkmp_init_one (struct pci_dev *dev,
				          const struct pci_device_id *ent)
{
	if (pci_enable_device(dev)) {
		printk("error enabling pci device %p\n", dev);
		return -EIO;
	}
	device_init( ++synclinkmp_adapter_count, dev );
	return 0;
}

static void __devexit synclinkmp_remove_one (struct pci_dev *dev)
{
}
