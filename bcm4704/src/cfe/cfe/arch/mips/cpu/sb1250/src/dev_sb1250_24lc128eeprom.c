/*  *********************************************************************
    *  Broadcom Common Firmware Environment (CFE)
    *  
    *  Microchip 24lc128 EEPROM driver 	File: dev_sb1250_24lc128eeprom.c
    *  
    *  This module contains a CFE driver for a Microchip 24LC128 EEPROM
    *  
    *  Author:  Mitch Lichtenberg (mpl@broadcom.com)
    *  
    *********************************************************************  
    *
    *  Copyright 2000,2001,2002,2003
    *  Broadcom Corporation. All rights reserved.
    *  
    *  This software is furnished under license and may be used and 
    *  copied only in accordance with the following terms and 
    *  conditions.  Subject to these conditions, you may download, 
    *  copy, install, use, modify and distribute modified or unmodified 
    *  copies of this software in source and/or binary form.  No title 
    *  or ownership is transferred hereby.
    *  
    *  1) Any source code used, modified or distributed must reproduce 
    *     and retain this copyright notice and list of conditions 
    *     as they appear in the source file.
    *  
    *  2) No right is granted to use any trade name, trademark, or 
    *     logo of Broadcom Corporation.  The "Broadcom Corporation" 
    *     name may not be used to endorse or promote products derived 
    *     from this software without the prior written permission of 
    *     Broadcom Corporation.
    *  
    *  3) THIS SOFTWARE IS PROVIDED "AS-IS" AND ANY EXPRESS OR
    *     IMPLIED WARRANTIES, INCLUDING BUT NOT LIMITED TO, ANY IMPLIED
    *     WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR 
    *     PURPOSE, OR NON-INFRINGEMENT ARE DISCLAIMED. IN NO EVENT 
    *     SHALL BROADCOM BE LIABLE FOR ANY DAMAGES WHATSOEVER, AND IN 
    *     PARTICULAR, BROADCOM SHALL NOT BE LIABLE FOR DIRECT, INDIRECT,
    *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
    *     (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
    *     GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
    *     BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY 
    *     OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR 
    *     TORT (INCLUDING NEGLIGENCE OR OTHERWISE), EVEN IF ADVISED OF 
    *     THE POSSIBILITY OF SUCH DAMAGE.
    ********************************************************************* */


#include "sbmips.h"
#include "lib_types.h"
#include "lib_malloc.h"
#include "lib_printf.h"

#include "cfe_iocb.h"
#include "cfe_device.h"
#include "cfe_ioctl.h"
#include "cfe_timer.h"

#include "sb1250_defs.h"
#include "sb1250_regs.h"
#include "sb1250_smbus.h"


/*  *********************************************************************
    *  Forward Declarations
    ********************************************************************* */

static void sb1250_24lc128eeprom_probe(cfe_driver_t *drv,
				     unsigned long probe_a, unsigned long probe_b, 
				     void *probe_ptr);


static int sb1250_24lc128eeprom_open(cfe_devctx_t *ctx);
static int sb1250_24lc128eeprom_read(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int sb1250_24lc128eeprom_inpstat(cfe_devctx_t *ctx,iocb_inpstat_t *inpstat);
static int sb1250_24lc128eeprom_write(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int sb1250_24lc128eeprom_ioctl(cfe_devctx_t *ctx,iocb_buffer_t *buffer);
static int sb1250_24lc128eeprom_close(cfe_devctx_t *ctx);

/*  *********************************************************************
    *  Dispatch tables
    ********************************************************************* */

#define M24LC128_EEPROM_SIZE	16384

const static cfe_devdisp_t sb1250_24lc128eeprom_dispatch = {
    sb1250_24lc128eeprom_open,
    sb1250_24lc128eeprom_read,
    sb1250_24lc128eeprom_inpstat,
    sb1250_24lc128eeprom_write,
    sb1250_24lc128eeprom_ioctl,
    sb1250_24lc128eeprom_close,
    NULL,
    NULL
};

const cfe_driver_t sb1250_24lc128eeprom = {
    "Microchip 24LC128 EEPROM",
    "eeprom",
    CFE_DEV_NVRAM,
    &sb1250_24lc128eeprom_dispatch,
    sb1250_24lc128eeprom_probe
};

typedef struct sb1250_24lc128eeprom_s {
    int smbus_channel;
    int smbus_address;
    int env_offset;
    int env_size;
    unsigned char data[M24LC128_EEPROM_SIZE];
} sb1250_24lc128eeprom_t;


/*  *********************************************************************
    *  smbus_init(chan)
    *  
    *  Initialize the specified SMBus channel
    *  
    *  Input parameters: 
    *  	   chan - channel # (0 or 1)
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */

static void smbus_init(int chan)
{
    uintptr_t reg;

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_FREQ));

    SBWRITECSR(reg,K_SMB_FREQ_100KHZ);	

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_CONTROL));

    SBWRITECSR(reg,0);	/* not in direct mode, no interrupts, will poll */
    
}

/*  *********************************************************************
    *  smbus_waitready(chan)
    *  
    *  Wait until the SMBus channel is ready.  We simply poll
    *  the busy bit until it clears.
    *  
    *  Input parameters: 
    *  	   chan - channel (0 or 1)
    *
    *  Return value:
    *      nothing
    ********************************************************************* */
static int smbus_waitready(int chan)
{
    uintptr_t reg;
    uint64_t status;

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_STATUS));

    for (;;) {
	status = SBREADCSR(reg);
	if (status & M_SMB_BUSY) continue;
	break;
	}

    if (status & M_SMB_ERROR) {
	
	SBWRITECSR(reg,(status & M_SMB_ERROR));
	return -1;
	}
    return 0;
}

/*  *********************************************************************
    *  smbus_readbyte(chan,slaveaddr,devaddr)
    *  
    *  Read a byte from the chip.  The 'slaveaddr' parameter determines
    *  whether we're reading from the RTC section or the EEPROM section.
    *  
    *  Input parameters: 
    *  	   chan - SMBus channel
    *  	   slaveaddr -  SMBus slave address
    *  	   devaddr - byte with in the device to read
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   else -1
    ********************************************************************* */

static int smbus_readbyte(int chan,int slaveaddr,int devaddr)
{
    uintptr_t reg;
    int err;

    /*
     * Make sure the bus is idle (probably should
     * ignore error here)
     */

    if (smbus_waitready(chan) < 0) return -1;

    /*
     * Write the device address to the controller. There are two
     * parts, the high part goes in the "CMD" field, and the 
     * low part is the data field.
     */

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_CMD));
    SBWRITECSR(reg,((devaddr >> 8) & 0x3F));

    /*
     * Write the data to the controller
     */

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_DATA));
    SBWRITECSR(reg,((devaddr & 0xFF) & 0xFF));

    /*
     * Start the command
     */

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_START));
    SBWRITECSR(reg,V_SMB_TT(K_SMB_TT_WR2BYTE) | slaveaddr);

    /*
     * Wait till done
     */

    err = smbus_waitready(chan);
    if (err < 0) return err;

    /*
     * Read the data byte
     */

    SBWRITECSR(reg,V_SMB_TT(K_SMB_TT_RD1BYTE) | slaveaddr);

    err = smbus_waitready(chan);
    if (err < 0) return err;

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_DATA));
    err = SBREADCSR(reg);

    return (err & 0xFF);
}

/*  *********************************************************************
    *  smbus_writebyte(chan,slaveaddr,devaddr,b)
    *  
    *  write a byte from the chip.  The 'slaveaddr' parameter determines
    *  whethe we're writing to the RTC section or the EEPROM section.
    *  
    *  Input parameters: 
    *  	   chan - SMBus channel
    *  	   slaveaddr -  SMBus slave address
    *  	   devaddr - byte with in the device to read
    *      b - byte to write
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   else -1
    ********************************************************************* */


static int smbus_writebyte(int chan,int slaveaddr,int devaddr,int b)
{
    uintptr_t reg;
    int err;
    int64_t timer;

    /*
     * Make sure the bus is idle (probably should
     * ignore error here)
     */

    if (smbus_waitready(chan) < 0) return -1;

    /*
     * Write the device address to the controller. There are two
     * parts, the high part goes in the "CMD" field, and the 
     * low part is the data field.
     */

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_CMD));
    SBWRITECSR(reg,((devaddr >> 8) & 0x3F));

    /*
     * Write the data to the controller
     */

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_DATA));
    SBWRITECSR(reg,((devaddr & 0xFF) | ((b & 0xFF) << 8)));

    /*
     * Start the command.  Keep pounding on the device until it
     * submits or the timer expires, whichever comes first.  The
     * datasheet says writes can take up to 10ms, so we'll give it 500.
     */

    reg = PHYS_TO_K1(A_SMB_REGISTER(chan,R_SMB_START));
    SBWRITECSR(reg,V_SMB_TT(K_SMB_TT_WR3BYTE) | slaveaddr);

    /*
     * Wait till the SMBus interface is done
     */	

    err = smbus_waitready(chan);
    if (err < 0) return err;

    /*
     * Pound on the device with a quick command (R/W=0)
     * to poll for the write complete.  See sect 7.0 of the
     * 24LC128 manual.
     */

    TIMER_SET(timer,50);
    err = -1; 

    while (!TIMER_EXPIRED(timer)) {
	POLL();

	SBWRITECSR(reg,V_SMB_TT(K_SMB_TT_QUICKCMD) | slaveaddr);

	err = smbus_waitready(chan);
	if (err == 0) break;
	}

    return err;
}


/*  *********************************************************************
    *  sb1250_24lc128eeprom_probe(drv,a,b,ptr)
    *  
    *  Probe routine for this driver.  This routine creates the 
    *  local device context and attaches it to the driver list
    *  within CFE.
    *  
    *  Input parameters: 
    *  	   drv - driver handle
    *  	   a,b - probe hints (longs)
    *  	   ptr - probe hint (pointer)
    *  	   
    *  Return value:
    *  	   nothing
    ********************************************************************* */

static void sb1250_24lc128eeprom_probe(cfe_driver_t *drv,
				     unsigned long probe_a, unsigned long probe_b, 
				     void *probe_ptr)
{
    sb1250_24lc128eeprom_t *softc;
    char descr[80];

    softc = (sb1250_24lc128eeprom_t *) KMALLOC(sizeof(sb1250_24lc128eeprom_t),0);

    /*
     * Probe_a is the SMBus channel number
     * Probe_b is the SMBus device offset
     * Probe_ptr is unused.
     */

    softc->smbus_channel = (int)probe_a;
    softc->smbus_address = (int)probe_b;
    softc->env_offset  = 0;
    softc->env_size = M24LC128_EEPROM_SIZE;

    xsprintf(descr,"%s on SMBus channel %d dev 0x%02X",
	     drv->drv_description,(int)probe_a,(int)probe_b);
    cfe_attach(drv,softc,NULL,descr);
}



/*  *********************************************************************
    *  sb1250_24lc128eeprom_open(ctx)
    *  
    *  Open this device.  For the X1240, we do a quick test 
    *  read to be sure the device is out there.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   else error code
    ********************************************************************* */

static int sb1250_24lc128eeprom_open(cfe_devctx_t *ctx)
{
    sb1250_24lc128eeprom_t *softc = ctx->dev_softc;
    int b;

    smbus_init(softc->smbus_channel);

    b = smbus_readbyte(softc->smbus_channel,
		       softc->smbus_address,
		       0);

    return (b < 0) ? -1 : 0;
}

/*  *********************************************************************
    *  sb1250_24lc128eeprom_read(ctx,buffer)
    *  
    *  Read bytes from the device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   buffer - buffer descriptor (target buffer, length, offset)
    *  	   
    *  Return value:
    *  	   number of bytes read
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_24lc128eeprom_read(cfe_devctx_t *ctx,iocb_buffer_t *buffer)
{
    sb1250_24lc128eeprom_t *softc = ctx->dev_softc;
    unsigned char *bptr;
    int blen;
    int idx;
    int b = 0;

    bptr = buffer->buf_ptr;
    blen = buffer->buf_length;

    if ((buffer->buf_offset + blen) > M24LC128_EEPROM_SIZE) return -1;

    idx = (int) buffer->buf_offset;

    while (blen > 0) {
	b = smbus_readbyte(softc->smbus_channel,
			  softc->smbus_address,
			  idx);
	if (b < 0) break;
	*bptr++ = (unsigned char) b;
	blen--;
	idx++;
	}

    buffer->buf_retlen = bptr - buffer->buf_ptr;
    return (b < 0) ? -1 : 0;
}

/*  *********************************************************************
    *  sb1250_24lc128eeprom_inpstat(ctx,inpstat)
    *  
    *  Test input (read) status for the device
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   inpstat - input status descriptor to receive value
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_24lc128eeprom_inpstat(cfe_devctx_t *ctx,iocb_inpstat_t *inpstat)
{
    inpstat->inp_status = 1;

    return 0;
}

/*  *********************************************************************
    *  sb1250_24lc128eeprom_write(ctx,buffer)
    *  
    *  Write bytes from the device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   buffer - buffer descriptor (target buffer, length, offset)
    *  	   
    *  Return value:
    *  	   number of bytes read
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_24lc128eeprom_write(cfe_devctx_t *ctx,iocb_buffer_t *buffer)
{
    sb1250_24lc128eeprom_t *softc = ctx->dev_softc;
    unsigned char *bptr;
    int blen;
    int idx;
    int b = 0;

    bptr = buffer->buf_ptr;
    blen = buffer->buf_length;

    if ((buffer->buf_offset + blen) > M24LC128_EEPROM_SIZE) return -1;

    idx = (int) buffer->buf_offset;

    while (blen > 0) {
	b = *bptr++;
	b = smbus_writebyte(softc->smbus_channel,
			   softc->smbus_address,
			   idx,
			   b);
	if (b < 0) break;
	blen--;
	idx++;
	}

    buffer->buf_retlen = bptr - buffer->buf_ptr;
    return (b < 0) ? -1 : 0;
}

/*  *********************************************************************
    *  sb1250_24lc128eeprom_ioctl(ctx,buffer)
    *  
    *  Perform miscellaneous I/O control operations on the device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   buffer - buffer descriptor (target buffer, length, offset)
    *  	   
    *  Return value:
    *  	   number of bytes read
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_24lc128eeprom_ioctl(cfe_devctx_t *ctx,iocb_buffer_t *buffer) 
{
    sb1250_24lc128eeprom_t *softc = ctx->dev_softc;
    nvram_info_t *info;

    switch ((int)buffer->buf_ioctlcmd) {
	case IOCTL_NVRAM_GETINFO:
	    info = (nvram_info_t *) buffer->buf_ptr;
	    if (buffer->buf_length != sizeof(nvram_info_t)) return -1;
	    info->nvram_offset = softc->env_offset;
	    info->nvram_size =   softc->env_size;
	    info->nvram_eraseflg = FALSE;
	    buffer->buf_retlen = sizeof(nvram_info_t);
	    return 0;
	default:
	    return -1;
	}
}

/*  *********************************************************************
    *  sb1250_24lc128eeprom_close(ctx,buffer)
    *  
    *  Close the device.
    *  
    *  Input parameters: 
    *  	   ctx - device context (can obtain our softc here)
    *  	   
    *  Return value:
    *  	   0 if ok
    *  	   -1 if an error occured
    ********************************************************************* */

static int sb1250_24lc128eeprom_close(cfe_devctx_t *ctx)
{
    return 0;
}


