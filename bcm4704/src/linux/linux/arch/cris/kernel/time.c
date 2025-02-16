/* $Id: time.c,v 1.1.1.1 2010/03/05 07:31:14 reynolds Exp $
 *
 *  linux/arch/cris/kernel/time.c
 *
 *  Copyright (C) 1991, 1992, 1995  Linus Torvalds
 *  Copyright (C) 1999, 2000, 2001 Axis Communications AB
 *
 * 1994-07-02    Alan Modra
 *	fixed set_rtc_mmss, fixed time.year for >= 2000, new mktime
 * 1995-03-26    Markus Kuhn
 *      fixed 500 ms bug at call to set_rtc_mmss, fixed DS12887
 *      precision CMOS clock update
 * 1996-05-03    Ingo Molnar
 *      fixed time warps in do_[slow|fast]_gettimeoffset()
 * 1997-09-10	Updated NTP code according to technical memorandum Jan '96
 *		"A Kernel Model for Precision Timekeeping" by Dave Mills
 *
 * Linux/CRIS specific code:
 *
 * Authors:    Bjorn Wesen
 *             Johan Adolfsson  
 * 2002-03-04    Johan Adolfsson
 *      Use prescale timer at 25000 Hz instead of the baudrate timer at 
 *      19200 to get rid of the 64ppm to fast timer (and we get better 
 *      resolution within a jiffie as well.
 * 2002-03-05    Johan Adolfsson
 *      Use prescaler in do_slow_gettimeoffset() to get 1 us resolution (40ns)
 *
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/delay.h>

#include <asm/segment.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/delay.h>
#include <asm/rtc.h>

#include <linux/timex.h>
#include <linux/config.h>

#include <asm/svinto.h>

#define CRIS_TEST_TIMERS 0

static int have_rtc;  /* used to remember if we have an RTC or not */

/* define this if you need to use print_timestamp */
/* it will make jiffies at 96 hz instead of 100 hz though */
#undef USE_CASCADE_TIMERS

extern int setup_etrax_irq(int, struct irqaction *);

#define TICK_SIZE tick

/* The timers count from their initial value down to 1 
 * The R_TIMER0_DATA counts down when R_TIM_PRESC_STATUS reaches halv
 * of the divider value.
 */ 
unsigned long get_ns_in_jiffie(void)
{
	unsigned char timer_count, t1;
	unsigned short presc_count;
	unsigned long ns;
	unsigned long flags;

	save_flags(flags);
	cli();	
	timer_count = *R_TIMER0_DATA;
	presc_count = *R_TIM_PRESC_STATUS;  
	/* presc_count might be wrapped */
	t1 = *R_TIMER0_DATA;

	if (timer_count != t1){
		/* it wrapped, read prescaler again...  */
		presc_count = *R_TIM_PRESC_STATUS;
		timer_count = t1;
	}
	restore_flags(flags);
	if (presc_count >= PRESCALE_VALUE/2 ){
		presc_count =  PRESCALE_VALUE - presc_count + PRESCALE_VALUE/2;
	} else {
		presc_count =  PRESCALE_VALUE - presc_count - PRESCALE_VALUE/2;
	}

	ns = ( (TIMER0_DIV - timer_count) * ((1000000000/HZ)/TIMER0_DIV )) + 
	     ( (presc_count) * (1000000000/PRESCALE_FREQ));
	return ns;
}



#if CRIS_TEST_TIMERS 
#define NS_TEST_SIZE 4000
static unsigned long ns_test[NS_TEST_SIZE];
void cris_test_timers(void)
{
	int i;
	for (i = 0; i < NS_TEST_SIZE; i++)
	{
		ns_test[i] = get_ns_in_jiffie();
	}

	for (i = 1; i < NS_TEST_SIZE; i++)
	{
		printk("%4i. %09lu ns diff %li ns\n",
		       i, ns_test[i], ns_test[i]- ns_test[i-1]);
	}
}

#endif

static unsigned long do_slow_gettimeoffset(void)
{
	unsigned long count, t1;
	unsigned long usec_count = 0;
	unsigned short presc_count;

	static unsigned long count_p = TIMER0_DIV;/* for the first call after boot */
	static unsigned long jiffies_p = 0;

	/*
	 * cache volatile jiffies temporarily; we have IRQs turned off. 
	 */
	unsigned long jiffies_t;

	/* The timer interrupt comes from Etrax timer 0. In order to get
	 * better precision, we check the current value. It might have
	 * underflowed already though.
	 */

#ifndef CONFIG_SVINTO_SIM
	/* Not available in the xsim simulator. */
	count = *R_TIMER0_DATA;
	presc_count = *R_TIM_PRESC_STATUS;  
	/* presc_count might be wrapped */
	t1 = *R_TIMER0_DATA;
	if (count != t1){
		/* it wrapped, read prescaler again...  */
		presc_count = *R_TIM_PRESC_STATUS;
		count = t1;
	}
#else
	count = 0;
	presc_count = 0;
#endif

 	jiffies_t = jiffies;

	/*
	 * avoiding timer inconsistencies (they are rare, but they happen)...
	 * there are three kinds of problems that must be avoided here:
	 *  1. the timer counter underflows
	 *  2. we are after the timer interrupt, but the bottom half handler
	 *     hasn't executed yet.
	 */
	if( jiffies_t == jiffies_p ) {
		if( count > count_p ) {
			/* Timer wrapped, use new count and prescale 
			 * increase the time corresponding to one jiffie
			 */
			usec_count = 1000000/HZ;
		}
	} else
		jiffies_p = jiffies_t;
        count_p = count;
	if (presc_count >= PRESCALE_VALUE/2 ){
		presc_count =  PRESCALE_VALUE - presc_count + PRESCALE_VALUE/2;
	} else {
		presc_count =  PRESCALE_VALUE - presc_count - PRESCALE_VALUE/2;
	}
	/* Convert timer value to usec */
	usec_count += ( (TIMER0_DIV - count) * (1000000/HZ)/TIMER0_DIV ) +
	              (( (presc_count) * (1000000000/PRESCALE_FREQ))/1000);

	return usec_count;
}

static unsigned long (*do_gettimeoffset)(void) = do_slow_gettimeoffset;

/*
 * This version of gettimeofday has near microsecond resolution.
 */
void do_gettimeofday(struct timeval *tv)
{
	unsigned long flags;

	save_flags(flags);
	cli();
	*tv = xtime;
	tv->tv_usec += do_gettimeoffset();
	restore_flags(flags);
	while (tv->tv_usec >= 1000000) {
		tv->tv_usec -= 1000000;
		tv->tv_sec++;
	}
}

void do_settimeofday(struct timeval *tv)
{
	cli();
	/* This is revolting. We need to set the xtime.tv_usec
	 * correctly. However, the value in this location is
	 * is value at the last tick.
	 * Discover what correction gettimeofday
	 * would have done, and then undo it!
	 */
	tv->tv_usec -= do_gettimeoffset();

	if (tv->tv_usec < 0) {
		tv->tv_usec += 1000000;
		tv->tv_sec--;
	}

	xtime = *tv;
	time_adjust = 0;		/* stop active adjtime() */
	time_status |= STA_UNSYNC;
	time_state = TIME_ERROR;	/* p. 24, (a) */
	time_maxerror = NTP_PHASE_LIMIT;
	time_esterror = NTP_PHASE_LIMIT;
	sti();
}


/*
 * BUG: This routine does not handle hour overflow properly; it just
 *      sets the minutes. Usually you'll only notice that after reboot!
 */

static int set_rtc_mmss(unsigned long nowtime)
{
	int retval = 0;
	int real_seconds, real_minutes, cmos_minutes;

	printk("set_rtc_mmss(%lu)\n", nowtime);

	if(!have_rtc)
		return 0;

	cmos_minutes = CMOS_READ(RTC_MINUTES);
	BCD_TO_BIN(cmos_minutes);

	/*
	 * since we're only adjusting minutes and seconds,
	 * don't interfere with hour overflow. This avoids
	 * messing with unknown time zones but requires your
	 * RTC not to be off by more than 15 minutes
	 */
	real_seconds = nowtime % 60;
	real_minutes = nowtime / 60;
	if (((abs(real_minutes - cmos_minutes) + 15)/30) & 1)
		real_minutes += 30;		/* correct for half hour time zone */
	real_minutes %= 60;

	if (abs(real_minutes - cmos_minutes) < 30) {
		BIN_TO_BCD(real_seconds);
		BIN_TO_BCD(real_minutes);
		CMOS_WRITE(real_seconds,RTC_SECONDS);
		CMOS_WRITE(real_minutes,RTC_MINUTES);
	} else {
		printk(KERN_WARNING
		       "set_rtc_mmss: can't update from %d to %d\n",
		       cmos_minutes, real_minutes);
		retval = -1;
	}

	return retval;
}

/* Excerpt from the Etrax100 HSDD about the built-in watchdog:
 *
 * 3.10.4 Watchdog timer

 * When the watchdog timer is started, it generates an NMI if the watchdog
 * isn't restarted or stopped within 0.1 s. If it still isn't restarted or
 * stopped after an additional 3.3 ms, the watchdog resets the chip.
 * The watchdog timer is stopped after reset. The watchdog timer is controlled
 * by the R_WATCHDOG register. The R_WATCHDOG register contains an enable bit
 * and a 3-bit key value. The effect of writing to the R_WATCHDOG register is
 * described in the table below:
 * 
 *   Watchdog    Value written:
 *   state:      To enable:  To key:      Operation:
 *   --------    ----------  -------      ----------
 *   stopped         0         X          No effect.
 *   stopped         1       key_val      Start watchdog with key = key_val.
 *   started         0       ~key         Stop watchdog
 *   started         1       ~key         Restart watchdog with key = ~key.
 *   started         X       new_key_val  Change key to new_key_val.
 * 
 * Note: '~' is the bitwise NOT operator.
 * 
 */

/* right now, starting the watchdog is the same as resetting it */
#define start_watchdog reset_watchdog

#if defined(CONFIG_ETRAX_WATCHDOG) && !defined(CONFIG_SVINTO_SIM)
static int watchdog_key = 0;  /* arbitrary number */
#endif

/* number of pages to consider "out of memory". it is normal that the memory
 * is used though, so put this really low.
 */

#define WATCHDOG_MIN_FREE_PAGES 8

void
reset_watchdog(void)
{
#if defined(CONFIG_ETRAX_WATCHDOG) && !defined(CONFIG_SVINTO_SIM)
	/* only keep watchdog happy as long as we have memory left! */
	if(nr_free_pages() > WATCHDOG_MIN_FREE_PAGES) {
		/* reset the watchdog with the inverse of the old key */
		watchdog_key ^= 0x7; /* invert key, which is 3 bits */
		*R_WATCHDOG = IO_FIELD(R_WATCHDOG, key, watchdog_key) |
			IO_STATE(R_WATCHDOG, enable, start);
	}
#endif
}

/* stop the watchdog - we still need the correct key */

void 
stop_watchdog(void)
{
#if defined(CONFIG_ETRAX_WATCHDOG) && !defined(CONFIG_SVINTO_SIM)
	watchdog_key ^= 0x7; /* invert key, which is 3 bits */
	*R_WATCHDOG = IO_FIELD(R_WATCHDOG, key, watchdog_key) |
		IO_STATE(R_WATCHDOG, enable, stop);
#endif	
}

/* last time the cmos clock got updated */
static long last_rtc_update = 0;

/*
 * timer_interrupt() needs to keep up the real-time clock,
 * as well as call the "do_timer()" routine every clocktick
 */

//static unsigned short myjiff; /* used by our debug routine print_timestamp */

static inline void
timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	/* acknowledge the timer irq */

#ifdef USE_CASCADE_TIMERS
	*R_TIMER_CTRL =
		IO_FIELD( R_TIMER_CTRL, timerdiv1, 0) |
		IO_FIELD( R_TIMER_CTRL, timerdiv0, 0) |
		IO_STATE( R_TIMER_CTRL, i1, clr) |
		IO_STATE( R_TIMER_CTRL, tm1, run) |
		IO_STATE( R_TIMER_CTRL, clksel1, cascade0) |
		IO_STATE( R_TIMER_CTRL, i0, clr) |
		IO_STATE( R_TIMER_CTRL, tm0, run) |
		IO_STATE( R_TIMER_CTRL, clksel0, c6250kHz);
#else
	*R_TIMER_CTRL = r_timer_ctrl_shadow | 
		IO_STATE(R_TIMER_CTRL, i0, clr);
#endif

	/* reset watchdog otherwise it resets us! */

	reset_watchdog();
	
	/* call the real timer interrupt handler */

	do_timer(regs);
	
	/*
	 * If we have an externally synchronized Linux clock, then update
	 * CMOS clock accordingly every ~11 minutes. Set_rtc_mmss() has to be
	 * called as close as possible to 500 ms before the new second starts.
	 */

	if ((time_status & STA_UNSYNC) == 0 &&
	    xtime.tv_sec > last_rtc_update + 660 &&
	    xtime.tv_usec > 500000 - (tick >> 1) &&
	    xtime.tv_usec < 500000 + (tick >> 1)) {
		if (set_rtc_mmss(xtime.tv_sec) == 0)
			last_rtc_update = xtime.tv_sec;
		else
			last_rtc_update = xtime.tv_sec - 600;
	}
}


/* grab the time from the RTC chip */

unsigned long
get_cmos_time(void)
{
	unsigned int year, mon, day, hour, min, sec;

	sec = CMOS_READ(RTC_SECONDS);
	min = CMOS_READ(RTC_MINUTES);
	hour = CMOS_READ(RTC_HOURS);
	day = CMOS_READ(RTC_DAY_OF_MONTH);
	mon = CMOS_READ(RTC_MONTH);
	year = CMOS_READ(RTC_YEAR);

	printk("rtc: sec 0x%x min 0x%x hour 0x%x day 0x%x mon 0x%x year 0x%x\n", 
	       sec, min, hour, day, mon, year);

	BCD_TO_BIN(sec);
	BCD_TO_BIN(min);
	BCD_TO_BIN(hour);
	BCD_TO_BIN(day);
	BCD_TO_BIN(mon);
	BCD_TO_BIN(year);

	if ((year += 1900) < 1970)
		year += 100;

	return mktime(year, mon, day, hour, min, sec);
}

/* update xtime from the CMOS settings. used when /dev/rtc gets a SET_TIME.
 * TODO: this doesn't reset the fancy NTP phase stuff as do_settimeofday does.
 */

void
update_xtime_from_cmos(void)
{
	if(have_rtc) {
		xtime.tv_sec = get_cmos_time();
		xtime.tv_usec = 0;
	}
}

/* timer is SA_SHIRQ so drivers can add stuff to the timer irq chain
 * it needs to be SA_INTERRUPT to make the jiffies update work properly
 */

static struct irqaction irq2  = { timer_interrupt, SA_SHIRQ | SA_INTERRUPT,
				  0, "timer", NULL, NULL};

void __init
time_init(void)
{	
	/* probe for the RTC and read it if it exists */

	if(RTC_INIT() < 0) {
		/* no RTC, start at 1980 */
		xtime.tv_sec = 0;
		xtime.tv_usec = 0;
		have_rtc = 0;
	} else {		
		/* get the current time */
		have_rtc = 1;
		update_xtime_from_cmos();
	}

	/* Setup the etrax timers
	 * Base frequency is 19200 hz, divider 192 -> 100 hz as Linux wants
	 * In normal mode, we use timer0, so timer1 is free. In cascade
	 * mode (which we sometimes use for debugging) both timers are used.
	 * Remember that linux/timex.h contains #defines that rely on the
	 * timer settings below (hz and divide factor) !!!
	 */
	
#ifdef USE_CASCADE_TIMERS
	*R_TIMER_CTRL =
		IO_FIELD( R_TIMER_CTRL, timerdiv1, 0) |
		IO_FIELD( R_TIMER_CTRL, timerdiv0, 0) |
		IO_STATE( R_TIMER_CTRL, i1, nop) |
		IO_STATE( R_TIMER_CTRL, tm1, stop_ld) |
		IO_STATE( R_TIMER_CTRL, clksel1, cascade0) |
		IO_STATE( R_TIMER_CTRL, i0, nop) |
		IO_STATE( R_TIMER_CTRL, tm0, stop_ld) |
		IO_STATE( R_TIMER_CTRL, clksel0, c6250kHz);
	
	*R_TIMER_CTRL = r_timer_ctrl_shadow = 
		IO_FIELD( R_TIMER_CTRL, timerdiv1, 0) |
		IO_FIELD( R_TIMER_CTRL, timerdiv0, 0) |
		IO_STATE( R_TIMER_CTRL, i1, nop) |
		IO_STATE( R_TIMER_CTRL, tm1, run) |
		IO_STATE( R_TIMER_CTRL, clksel1, cascade0) |
		IO_STATE( R_TIMER_CTRL, i0, nop) |
		IO_STATE( R_TIMER_CTRL, tm0, run) |
		IO_STATE( R_TIMER_CTRL, clksel0, c6250kHz);
#else
  
	*R_TIMER_CTRL = 
		IO_FIELD(R_TIMER_CTRL, timerdiv1, 192)      | 
		IO_FIELD(R_TIMER_CTRL, timerdiv0, TIMER0_DIV)      |
		IO_STATE(R_TIMER_CTRL, i1,        nop)      | 
		IO_STATE(R_TIMER_CTRL, tm1,       stop_ld)  |
		IO_STATE(R_TIMER_CTRL, clksel1,   c19k2Hz)  |
		IO_STATE(R_TIMER_CTRL, i0,        nop)      |
		IO_STATE(R_TIMER_CTRL, tm0,       stop_ld)  |
		IO_STATE(R_TIMER_CTRL, clksel0,   flexible);
	
	*R_TIMER_CTRL = r_timer_ctrl_shadow =
		IO_FIELD(R_TIMER_CTRL, timerdiv1, 192)      | 
		IO_FIELD(R_TIMER_CTRL, timerdiv0, TIMER0_DIV)      |
		IO_STATE(R_TIMER_CTRL, i1,        nop)      |
		IO_STATE(R_TIMER_CTRL, tm1,       run)      |
		IO_STATE(R_TIMER_CTRL, clksel1,   c19k2Hz)  |
		IO_STATE(R_TIMER_CTRL, i0,        nop)      |
		IO_STATE(R_TIMER_CTRL, tm0,       run)      |
		IO_STATE(R_TIMER_CTRL, clksel0,   flexible);

	*R_TIMER_PRESCALE = PRESCALE_VALUE;
#endif

#if CRIS_TEST_TIMERS
	cris_test_timers();
#endif
	
	*R_IRQ_MASK0_SET =
		IO_STATE(R_IRQ_MASK0_SET, timer0, set); /* unmask the timer irq */
	
	/* now actually register the timer irq handler that calls timer_interrupt() */
	
	setup_etrax_irq(2, &irq2); /* irq 2 is the timer0 irq in etrax */

	/* enable watchdog if we should use one */

#if defined(CONFIG_ETRAX_WATCHDOG) && !defined(CONFIG_SVINTO_SIM)
	printk("Enabling watchdog...\n");
	start_watchdog();

	/* If we use the hardware watchdog, we want to trap it as an NMI
	   and dump registers before it resets us.  For this to happen, we
	   must set the "m" NMI enable flag (which once set, is unset only
	   when an NMI is taken).

	   The same goes for the external NMI, but that doesn't have any
	   driver or infrastructure support yet.  */
	asm ("setf m");

	*R_IRQ_MASK0_SET =
		IO_STATE(R_IRQ_MASK0_SET, watchdog_nmi, set);
	*R_VECT_MASK_SET =
		IO_STATE(R_VECT_MASK_SET, nmi, set);
#endif
}
