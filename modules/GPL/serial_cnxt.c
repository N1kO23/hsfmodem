/*
*  Virtual serial port driver for Conexant modems
*
*	Written by Marc Boucher <marc@linuxant.com>
*/

/*
* Copyright (c) 2001-2002 Conexant Systems, Inc.
* Copyright (c) 2003-2004 Linuxant inc.
* 
* 1.  General Public License. This program is free software, and may
* be redistributed or modified subject to the terms of the GNU General
* Public License (version 2) or the GNU Lesser General Public License,
* or (at your option) any later versions ("Open Source" code). You may
* obtain a copy of the GNU General Public License at
* http://www.fsf.org/copyleft/gpl.html and a copy of the GNU Lesser
* General Public License at http://www.fsf.org/copyleft/less.html,
* or you may alternatively write to the Free Software Foundation, Inc.,
* 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA.
* 
* 2.   Disclaimer of Warranties. CONEXANT AND OTHER CONTRIBUTORS MAKE NO
* REPRESENTATION ABOUT THE SUITABILITY OF THIS SOFTWARE FOR ANY PURPOSE.
* IT IS PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTIES OF ANY KIND.
* CONEXANT AND OTHER CONTRIBUTORS DISCLAIMS ALL WARRANTIES WITH REGARD TO
* THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
* FOR A PARTICULAR PURPOSE, GOOD TITLE AND AGAINST INFRINGEMENT.
* 
* This software has not been formally tested, and there is no guarantee that
* it is free of errors including, but not limited to, bugs, defects,
* interrupted operation, or unexpected results. Any use of this software is
* at user's own risk.
* 
* 3.   No Liability.
* 
* (a) Conexant or contributors shall not be responsible for any loss or
* damage to user, or any third parties for any reason whatsoever, and
* CONEXANT OR CONTRIBUTORS SHALL NOT BE LIABLE FOR ANY ACTUAL, DIRECT,
* INDIRECT, SPECIAL, PUNITIVE, INCIDENTAL, OR CONSEQUENTIAL
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
* HOWEVER CAUSED, WHETHER IN CONTRACT, STRICT OR OTHER LEGAL THEORY OF
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
* WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
* 
* (b) User agrees to hold Conexant and contributors harmless from any
* liability, loss, cost, damage or expense, including attorney's fees,
* as a result of any claims which may be made by any person, including
* but not limited to User, its agents and employees, its customers, or
* any third parties that arise out of or result from the manufacture,
* delivery, actual or alleged ownership, performance, use, operation
* or possession of the software furnished hereunder, whether such claims
* are based on negligence, breach of contract, absolute liability or any
* other legal theory.
* 
* 4.   Notices. User hereby agrees not to remove, alter or destroy any
* copyright, trademark, credits, other proprietary notices or confidential
* legends placed upon, contained within or associated with the Software,
* and shall include all such unaltered copyright, trademark, credits,
* other proprietary notices or confidential legends on or in every copy of
* the Software.
* 
*/
#include <linux/version.h>
#ifdef FOUND_LINUX_CONFIG
#include <linux/config.h>
#endif
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/major.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <asm/serial.h>
#include <linux/serial.h>

#if !(LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
#if (!defined(CONFIG_SERIAL_CORE) && !defined(CONFIG_SERIAL_CORE_MODULE))
#error CONFIG_SERIAL_CORE needed; enable 8250/16550 and compatible serial support in kernel config
#endif
#include <linux/serial_core.h>
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0))
#include <linux/seq_file.h>
#endif
#include <linux/irq.h>
#include <linux/platform_device.h>

#include "oscompat.h"
#include "osservices.h"
#include "comtypes.h"
#include "comctrl_ex.h"
#include "oslinux.h"
#include "osnvm.h"
#include "osstdio.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
#include "serial_core.h"
#endif

#include "serial_cnxt.h"

#define NR_PORTS		CNXTMAXMDM
#define CNXT_ISR_PASS_LIMIT	256
#define CNXT_READBUF_SIZE	256

#ifndef CNXTSERIAL_INCLUDE_CORE
	#define uart_init() 0
	#define uart_exit() {}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
static struct tty_driver cnxt_tty_driver_normal;
static struct tty_driver cnxt_tty_driver_callout;
static struct tty_struct *cnxt_tty_table[NR_PORTS];
static struct termios *cnxt_termios[NR_PORTS], *cnxt_termios_locked[NR_PORTS];
#endif

static struct uart_driver cnxt_reg = {
	.owner		=				THIS_MODULE,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	#ifdef CONFIG_DEVFS_FS
		.normal_name	=		"ttyS"CNXTSERDEV"%d",
		.callout_name	=		"cua"CNXTSERDEV"%d",
	#else
		.normal_name	=		"ttyS"CNXTSERDEV,
		.callout_name	=		"cua"CNXTSERDEV,
	#endif
	.normal_driver	=		&cnxt_tty_driver_normal,
	.callout_driver	=		&cnxt_tty_driver_callout,
	.table		=		cnxt_tty_table,
	.termios	=		cnxt_termios,
	.termios_locked	=		cnxt_termios_locked,
	.port		=		cnxt_ports,
#else
	.driver_name	=		CNXTTARGET"serial",
	#ifdef FOUND_DEVFS
		.devfs_name	=		"ttyS"CNXTSERDEV,
	#endif
	.dev_name	=		"ttyS"CNXTSERDEV,
#endif
	.minor		=		CNXTSERIALMINOR,
	.nr		=		NR_PORTS,
	.major = CNXTSERIALMAJOR,
};

struct cnxt_serial_inst {
	spinlock_t lock;
	struct module *owner;
	POS_DEVNODE devnode;
	HANDLE hcomctrl;
	char *typestr;
	struct uart_port *port;
	struct uart_port *uart_port;
	uart_info_t *uart_info;	
	u_int mctrl_flags;
	struct{
		unsigned int rxenabled:1;
		unsigned int txenabled:1;
		unsigned int transmit:2;
		unsigned int evt_rxchar:1;
		unsigned int evt_rxbreak:1;
		unsigned int evt_rxovrn:1;
		unsigned int evt_txempty:1;
		unsigned int evt_ring:1;		
	};
	unsigned char readbuf[CNXT_READBUF_SIZE];
	int readcount, readoffset;	
	struct{
		unsigned int dwEvtMask[10];
		unsigned char count;
		int user_pid;
	} signal;
	OSSCHED intr_tqueue;
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry *proc_unit_dir;
	struct proc_dir_entry *proc_hwinst;
	struct proc_dir_entry *proc_hwprofile;
	struct proc_dir_entry *proc_hwrevision;
#ifdef COMCTRL_MONITOR_POUND_UG_SUPPORT
	struct proc_dir_entry *proc_lastcallstatus;
#endif
#endif
};

#ifdef CONFIG_PROC_FS
static struct proc_dir_entry *cnxt_serial_proc_dir;
static struct proc_dir_entry *cnxt_serial_flush_nvm;
#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0) )

#endif
#endif

static struct cnxt_serial_inst *cnxt_serial_inst = NULL;
static struct uart_port *cnxt_ports = NULL;

#if LINUX_VERSION_CODE > KERNEL_VERSION(4,1,0)
static struct _cnxt_serial_shared_memory{
	int hwInstNum;
	unsigned char *buf;
} *cnxt_serial_shared_memory = NULL;
#endif

#ifdef COMCTRL_MONITOR_POUND_UG_SUPPORT
static int loglastcallstatus;
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 37)
static DECLARE_MUTEX(cnxt_port_sem);
#else
static DEFINE_SEMAPHORE(cnxt_port_sem);
#endif

//06/12/2018
static int uart_register_port(struct uart_driver *drv, struct uart_port *port);
static void uart_unregister_port(struct uart_driver *drv, int line);

static void cnxt_sched_intr(struct cnxt_serial_inst *inst){
	if(inst->uart_info) {
		OsModuleUseCountInc();
		if (OsThreadSchedule(OsMdmThread, &inst->intr_tqueue) <= 0)
			OsModuleUseCountDec();
	}
}

static COM_STATUS cnxt_control(struct cnxt_serial_inst *inst, COMCTRL_CONTROL_CODE eCode, PVOID pControl)
{
	int r;

	if(!inst->hcomctrl)
		return COM_STATUS_INST_NOT_INIT;
	r = ComCtrl_Control(inst->hcomctrl, eCode, pControl);
	if (r != COM_STATUS_SUCCESS)
		printk(KERN_ERR "%s: ComCtrlControl %d failed, status=%d\n", __FUNCTION__, eCode, r);
	return r;
}

static void
#ifdef FOUND_TTY_START_STOP
cnxt_stop_tx(struct uart_port *port, u_int tty_stop)
#else
cnxt_stop_tx(struct uart_port *port)
#endif
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];
	//printk(KERN_DEBUG "%s %d\n", __FUNCTION__,(int)(port - cnxt_ports));
	inst->txenabled = 0;
}

static void
#ifdef FOUND_TTY_START_STOP
cnxt_start_tx(struct uart_port *port, u_int tty_start)
#else
cnxt_start_tx(struct uart_port *port)
#endif
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];

	//printk(KERN_DEBUG "%s %p\n", __FUNCTION__,port);	
	
	cnxt_sched_intr(inst);
	inst->txenabled = 1;
	inst->rxenabled = 1;	
	inst->evt_txempty=0;
}

static void cnxt_stop_rx(struct uart_port *port)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];
		
	//cnxt_control(inst, COMCTRL_CONTROL_SET_BREAK_ON, 0);
	inst->rxenabled = 0;
}

static int cnxt_rx_ready(struct cnxt_serial_inst *inst)
{
	int r;
		
	if(!inst->rxenabled)
		return FALSE;	
	r=0;
	if(inst->readoffset == inst->readcount) {		
		r = ComCtrl_Read(inst->hcomctrl,inst->readbuf, sizeof(inst->readbuf));//c06419a5 c0605e89
		if(r < 0)
			printk(KERN_ERR"%s: ComCtrlRead returned %d\n", __FUNCTION__, r);
		else{
			inst->readcount = r;
			inst->readoffset = 0;			
			inst->evt_rxchar = 0;
		}
	}
	return (inst->readcount > inst->readoffset) || inst->evt_rxbreak || inst->evt_rxovrn;
}

static void cnxt_rx_chars(struct cnxt_serial_inst *inst){
	struct tty_struct *tty = UART_INFO_TO_TTY(inst->uart_info);//port->state->port.tty
	int max_count;
	unsigned char flag,ch;
	char lino[512];
	int i;
	spinlock_t *lock;
	unsigned long flags;
	struct uart_port *port;
	
	lino[i=0]=0;
	//inst->evt_rxchar = 0;	
	if(!inst->uart_port || !inst->readcount)
		return;
	port = inst->uart_port;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	lock = &info->lock;
#else
	lock = &port->lock;
#endif
	
	spin_lock_irqsave(lock, flags);		
	max_count = inst->readcount;
	while(max_count-- > 0) {
#ifndef FOUND_TTY_NEW_API //old vesrion
		if(unlikely(tty->flip.count >= TTY_FLIPBUF_SIZE)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
			tty->flip.tqueue.routine((void *) tty);
#else
			tty->flip.work.func((void *)tty);
#endif
			if(tty->flip.count >= TTY_FLIPBUF_SIZE){
				spin_unlock_irqrestore(lock, flags);				
				return; 
			}
		}
#endif
		if (inst->evt_rxovrn) {
			inst->evt_rxovrn = 0;
			inst->uart_port->icount.overrun++;
#ifdef FOUND_TTY_NEW_API
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0)
	    tty_insert_flip_char(tty->port, 0, TTY_OVERRUN);
#else
	    tty_insert_flip_char(tty, 0, TTY_OVERRUN);
#endif
#else
			*tty->flip.flag_buf_ptr++ = TTY_OVERRUN;
			*tty->flip.char_buf_ptr++ = 0;
			tty->flip.count++;
#endif
			continue;
		}
		
		port->icount.rx++;		
		
		if (inst->evt_rxbreak) {
			inst->evt_rxbreak = 0;
			port->icount.brk++;
			flag = TTY_BREAK;
		} 
		else
			flag = TTY_NORMAL;				
		ch = inst->readbuf[inst->readoffset++];
		if(i < 510)
			lino[i++]=ch;
#ifdef FOUND_TTY_NEW_API
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0)
	tty_insert_flip_char(tty->port, inst->readbuf[inst->readoffset++], flag);
#else
	tty_insert_flip_char(tty, inst->readbuf[inst->readoffset++], flag);
#endif
#else
	*tty->flip.flag_buf_ptr++ = flag;
	*tty->flip.char_buf_ptr++ = inst->readbuf[inst->readoffset++];
	tty->flip.count++;
#endif
    }

//     TODO is this needed?
// #if LINUX_VERSION_CODE >= KERNEL_VERSION(3,9,0)
//     tty_flip_buffer_push(tty->port);
// #else
//     tty_flip_buffer_push(tty);
// #endif
//     return;
	if(i){
		lino[i]=0;
		printk(KERN_DEBUG "%s %p: %s\n", __FUNCTION__, inst->port,lino);

		tty_flip_buffer_push(&inst->uart_info->port);
		//uart_write_wakeup(port);	
		inst->transmit |= 1;
	}		
	spin_unlock_irqrestore(lock, flags);				
}

static int cnxt_tx_free(struct cnxt_serial_inst *inst)
{
	int r;
	UINT32 val = 0;

	if ((r=ComCtrl_Monitor(inst->hcomctrl, COMCTRL_MONITOR_TXFREE, &val)) != COM_STATUS_SUCCESS) {
		printk(KERN_ERR "%s: ComCtrlMonitor COMCTRL_MONITOR_TXFREE failed, status=%d\n", __FUNCTION__, r);
		return 0;
	}
	return val;
}

static int cnxt_put_char(struct cnxt_serial_inst *inst, unsigned char ch)
{
	int r;

	//printk(KERN_DEBUG "%s: ch=%x\n", __FUNCTION__, (int)ch);
	inst->evt_txempty = 0;
	r = ComCtrl_Write(inst->hcomctrl, &ch, 1);
	if(r != 1)
		printk(KERN_ERR "%s: ComCtrlWrite returned %d\n", __FUNCTION__, r);
	return r == 1;
}

static void cnxt_tx_chars(struct cnxt_serial_inst *inst)
{
	struct circ_buf *xmit;
	uart_info_t *info = inst->uart_info;
	struct uart_port *port;
	spinlock_t *lock;
	unsigned long flags;
	unsigned int to_send, until_end;
	unsigned char ch;
	
	int i;
	char lino[512],s[5];
	
	lino[i=0]=0;
	port = inst->uart_port;
	xmit = &info->xmit;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	lock = &info->lock;
#else
	lock = &port->lock;
#endif
	
	spin_lock_irqsave(lock, flags);

	if (port->x_char) {
		cnxt_put_char(inst, port->x_char);
		port->icount.tx++;
		port->x_char = 0;
		spin_unlock_irqrestore(lock, flags);
		return;
	}
	
	if (uart_circ_empty(xmit) ||uart_tx_stopped(port)) {
#ifdef FOUND_TTY_START_STOP
		cnxt_stop_tx(port, 0);
#else
		cnxt_stop_tx(port);
#endif
		spin_unlock_irqrestore(lock, flags);
		return;
	}
	
	to_send = uart_circ_chars_pending(xmit);
	until_end = CIRC_CNT_TO_END(xmit->head, xmit->tail, UART_XMIT_SIZE);
	
	while (xmit->buf && to_send) {
		ch = xmit->buf[xmit->tail];
		sprintf(s,"%02X%c",ch,ch);
		strcat(lino,s);
		if(!cnxt_put_char(inst, ch))
			break;		
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		port->icount.tx++;
		to_send--;
	}
	//lino[i] = 0;
	printk(KERN_DEBUG "%s: %s %d %d\n", __FUNCTION__,lino,to_send,until_end);
	
	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS){
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
		uart_event(info, EVT_WRITE_WAKEUP);
#else
		uart_write_wakeup(port);
#endif
	}
	
	if (uart_circ_empty(xmit)){
		cnxt_tx_free(inst);

#ifdef FOUND_TTY_START_STOP
		cnxt_stop_tx(port, 0);
#else
		cnxt_stop_tx(port);
#endif
/*
#ifdef FOUND_NO_STRUCT_UART_INFO
			wake_up_interruptible(&info->port.delta_msr_wait);
#else
			wake_up_interruptible(&info->delta_msr_wait);
#endif*/
	}
	
	spin_unlock_irqrestore(lock, flags);	
}

static int cnxt_tx_ready(struct cnxt_serial_inst *inst) {
	return (inst->txenabled/* && !inst->evt_txempty*/);
}

//fixme
static void cnxt_intr(void *dev_id)
{
	int i;
	struct cnxt_serial_inst *inst = (struct cnxt_serial_inst *)dev_id;
	
	//printk(KERN_DEBUG "%s:\n", __FUNCTION__);
	inst->transmit=0;
	for (i = 0;i < CNXT_ISR_PASS_LIMIT && inst->uart_info && !inst->transmit;i++) {
		if(cnxt_tx_ready(inst))
			cnxt_tx_chars(inst);
		if(cnxt_rx_ready(inst))
			cnxt_rx_chars(inst);		
	}
	
	if(inst->signal.count){
		unsigned long flags;
		
		spin_lock_irqsave(&inst->lock, flags);
		
		unsigned int dwEvtMask = inst->signal.dwEvtMask[0];
		for(i=0,inst->signal.count--;i<inst->signal.count;i++)
			inst->signal.dwEvtMask[i] = inst->signal.dwEvtMask[i+1];
		
		spin_unlock_irqrestore(&inst->lock, flags);
		
		if(inst->signal.user_pid != 0){
			struct siginfo info={0};
			struct task_struct *t;
					
			rcu_read_lock();		
			t = pid_task(find_pid_ns(inst->signal.user_pid, &init_pid_ns), PIDTYPE_PID);
			if (t != NULL) {
				int err;
				
				info.si_signo = 44;
				info.si_code = SI_QUEUE;
				info.si_int = dwEvtMask;
				
				rcu_read_unlock();      
				if (err = send_sig_info(44, &info, t) < 0)
					printk("send_sig_info error %d\n",err);
			} 
			else {
				printk("pid_task error\n");
				inst->signal.user_pid = 0;
				rcu_read_unlock();
				//return -ENODEV;
			}
		}	
		
	}
	
	OsModuleUseCountDec();
	OsThreadScheduleDone();
}

static u_int cnxt_tx_empty(struct uart_port *port)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];
	//printk(KERN_ERR "%s: \n", __FUNCTION__);
	return inst->evt_txempty ? TIOCSER_TEMT : 0;
}

static u_int cnxt_get_mctrl(struct uart_port *port)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];
	printk(KERN_ERR "%s: \n", __FUNCTION__);
	return inst->mctrl_flags;
}

#ifdef COMCTRL_MONITOR_POUND_UG_SUPPORT
static COM_STATUS cnxt_monitor(struct cnxt_serial_inst *inst, COMCTRL_MONITOR_CODE eCode, PVOID pMonitor)
{
	int r;

	if(!inst->hcomctrl)
		return COM_STATUS_INST_NOT_INIT;
	r = ComCtrl_Monitor(inst->hcomctrl, eCode, pMonitor);
	if (r != COM_STATUS_SUCCESS)
		printk(KERN_ERR "%s: ComCtrlMonitor %d failed, status=%d\n", __FUNCTION__, eCode, r);
	return r;
}
#endif

static void cnxt_set_mctrl(struct uart_port *port, u_int mctrl)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];

	/*
#define TIOCM_LE	0x001
#define TIOCM_DTR	0x002
#define TIOCM_RTS	0x004
#define TIOCM_ST	0x008
#define TIOCM_SR	0x010
#define TIOCM_CTS	0x020
#define TIOCM_CAR	0x040
#define TIOCM_RNG	0x080
#define TIOCM_DSR	0x100
#define TIOCM_CD	TIOCM_CAR
#define TIOCM_RI	TIOCM_RNG
#define TIOCM_OUT1	0x2000
#define TIOCM_OUT2	0x4000
#define TIOCM_LOOP	0x8000
*/
	//printk(KERN_DEBUG "%s: mctrl=%08X omctrl=%08X\n", __FUNCTION__, mctrl,inst->mctrl_flags);
	
	if ((mctrl & TIOCM_RTS) && !(inst->mctrl_flags & TIOCM_RTS)) {//0x4
		inst->mctrl_flags |= TIOCM_RTS;
		//cnxt_control(inst, COMCTRL_CONTROL_SETRTS, 0);
	}
	
	if (!(mctrl & TIOCM_RTS) && (inst->mctrl_flags & TIOCM_RTS)) {			
		inst->mctrl_flags &= ~TIOCM_RTS;
		//cnxt_control(inst, COMCTRL_CONTROL_CLRRTS, 0);		
	}
	
	if ((mctrl & TIOCM_DTR) && !(inst->mctrl_flags & TIOCM_DTR)) {//0x2
		inst->mctrl_flags |= TIOCM_DTR;
		//cnxt_control(inst, COMCTRL_CONTROL_SETDTR, 0);
	}
	
	if (!(mctrl & TIOCM_DTR) && (inst->mctrl_flags & TIOCM_DTR)) {
		inst->mctrl_flags &= ~TIOCM_DTR;
//		cnxt_control(inst, COMCTRL_CONTROL_CLRDTR, 0);
	}	
}

static void cnxt_break_ctl(struct uart_port *port, int break_state)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];

	//printk(KERN_DEBUG "%s: break_state=%d\n", __FUNCTION__, break_state);
	cnxt_control(inst, break_state ? COMCTRL_CONTROL_SET_BREAK_ON : COMCTRL_CONTROL_SET_BREAK_OFF, 0);
}

__shimcall__ static void cnxt_event_handler(struct cnxt_serial_inst *inst, UINT32 dwEvtMask)
{
	struct uart_port *port = inst->port;
	u_int mctrl_flags, orig_mctrl_flags;
	int sched_intr=0;
	
	printk(KERN_DEBUG "%s %08X %p\n", __FUNCTION__,dwEvtMask,port,&cnxt_reg,&cnxt_ports[cnxt_serial_shared_memory->hwInstNum]);
	
	orig_mctrl_flags = mctrl_flags = inst->mctrl_flags;
	if((dwEvtMask & COMCTRL_EVT_RXCHAR)) {
		inst->evt_rxchar = 1;		
		sched_intr |= 3;		
	}

	if(dwEvtMask & COMCTRL_EVT_BREAK) {
		inst->evt_rxbreak = 1;
		sched_intr |= 1;		
	}

	if((dwEvtMask & COMCTRL_EVT_RXOVRN)) {
		inst->evt_rxovrn = 1;
		sched_intr = 1;
	}

	if((dwEvtMask & COMCTRL_EVT_TXCHAR)) {
	
	}

	if((dwEvtMask & COMCTRL_EVT_TXEMPTY)) {
		inst->evt_txempty = 1;
		sched_intr |= 3;
	}

	if((dwEvtMask & COMCTRL_EVT_CTS)) {
		if(dwEvtMask & COMCTRL_EVT_CTSS)
			mctrl_flags |= TIOCM_CTS;
		else
			mctrl_flags &= ~TIOCM_CTS;
	}

	if(dwEvtMask & COMCTRL_EVT_DSR) {
		if(dwEvtMask & COMCTRL_EVT_DSRS)
			mctrl_flags |= TIOCM_DSR;
		else
			mctrl_flags &= ~TIOCM_DSR;
		if(port)
			port->icount.dsr++;
	}

	if(dwEvtMask & COMCTRL_EVT_RLSD) {
		if(dwEvtMask & COMCTRL_EVT_RLSDS)
			mctrl_flags |= TIOCM_CAR;
		else
			mctrl_flags &= ~TIOCM_CAR;
	}

	if(dwEvtMask & COMCTRL_EVT_RING) {
		if(dwEvtMask & COMCTRL_EVT_RINGS) {
			mctrl_flags |= TIOCM_RNG;
			if(port)
				port->icount.rng++;
		} 
		else
			mctrl_flags &= ~TIOCM_RNG;
	}

	if(inst->mctrl_flags != mctrl_flags) {
		inst->mctrl_flags = mctrl_flags;
#if 0
		printk(KERN_DEBUG "%cCTS %cDSR %cDCD %cRI\n",
		inst->mctrl_flags&TIOCM_CTS?'+':'-',
		inst->mctrl_flags&TIOCM_DSR?'+':'-',
		inst->mctrl_flags&TIOCM_CAR?'+':'-',
		inst->mctrl_flags&TIOCM_RNG?'+':'-');
#endif

		if(port && inst->uart_info) {
			if((mctrl_flags & TIOCM_CAR) != (orig_mctrl_flags & TIOCM_CAR)) {
				uart_handle_dcd_change(
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
				inst->uart_info,
#else
				inst->uart_port,
#endif
				mctrl_flags & TIOCM_CAR);
#ifdef COMCTRL_MONITOR_POUND_UG_SUPPORT
				if(loglastcallstatus && !(mctrl_flags & TIOCM_CAR)) {
					PORT_MONITOR_DATA monitorData;
					static char largebuf[PAGE_SIZE];
					char *p, *nl;

					monitorData.dwSize = sizeof(largebuf);
					monitorData.pBuf = largebuf;

					cnxt_monitor(inst, COMCTRL_MONITOR_POUND_UG, &monitorData);
					p = largebuf;
					while((nl = strchr(p, '\n'))) {
						printk(KERN_INFO "%.*s", (int)(nl - p) + 1, p);
						p = nl + 1;
					}
					if(*p)
						printk(KERN_INFO "%s\n", p);
				}
#endif
			}
									
			if((mctrl_flags & TIOCM_CTS) != (orig_mctrl_flags & TIOCM_CTS))
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
				uart_handle_cts_change(inst->uart_info,mctrl_flags & TIOCM_CTS);
#else
				uart_handle_cts_change(inst->uart_port,mctrl_flags & TIOCM_CTS);
#endif
		}
	}
	//mutex
	if((sched_intr & 2) && inst->signal.count < sizeof(inst->signal.dwEvtMask) / sizeof(unsigned int))
		inst->signal.dwEvtMask[inst->signal.count++] = dwEvtMask;
	if(port && sched_intr)
		cnxt_sched_intr(inst);		
}

static irqreturn_t irq_uart_tx_chars(int irq, void *dev_id){	
	printk(KERN_DEBUG "%s: (%d)\n", __FUNCTION__, irq);
	return IRQ_HANDLED;
}

static int cnxt_startup(struct uart_port *port
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
, struct uart_info *info
#endif
)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];
	
	if(!inst->hcomctrl)
		return -ENODEV;
	if(inst->uart_info)
		return -EBUSY;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	if (!try_inc_mod_count(inst->owner))
		return -ENODEV;
#else
	if (!try_module_get(inst->owner))
		return -ENODEV;
#endif
		
	while(ComCtrl_Read(inst->hcomctrl, inst->readbuf, sizeof(inst->readbuf)) > 0);
	
	inst->readcount = inst->readoffset = 0;
	inst->evt_rxchar = 0;
	inst->evt_rxbreak = 0;
	inst->evt_rxovrn = 0;

	inst->uart_port = port;
	
//	int r = request_irq(inst->irq, irq_uart_tx_chars, IRQF_SHARED|IRQF_ONESHOT, "UART TX", port);
	
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	inst->uart_info = info;
#else	
	#ifdef FOUND_NO_STRUCT_UART_INFO
		inst->uart_info = port->state;
	#else
		inst->uart_info = port->info;
	#endif
#endif

	inst->rxenabled = 1;
	inst->txenabled = 1;
#ifdef USE_DCP
	OsDcpEnsureDaemonIsRunning(inst->devnode->hwInstNum);
#endif
	//printk(KERN_DEBUG "%s %p\n", __FUNCTION__,port);
	return 0;
}

static void cnxt_shutdown(struct uart_port *port
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
, struct uart_info *info
#endif
)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];

	printk(KERN_DEBUG "%s %p\n", __FUNCTION__,port);
	
	inst->rxenabled = 0;
	inst->txenabled = 0;

	inst->uart_info = NULL;
	inst->uart_port = NULL;

	if (inst->owner) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
		__MOD_DEC_USE_COUNT(inst->owner);
#else
		module_put(inst->owner);
#endif
	}
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
static void cnxt_change_speed(struct uart_port *port, u_int cflag, u_int iflag, u_int quot)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];
	PORT_CONFIG port_config;

	printk(KERN_DEBUG "%s\n", __FUNCTION__);

	memset(&port_config, 0, sizeof(port_config));
	if(quot) {
		port_config.dwDteSpeed = port->uartclk / (16 * quot);
		port_config.dwValidFileds |= PC_DTE_SPEED;
	}
	if(cflag & PARENB) {
		if(cflag & PARODD)
			port_config.eParity = PC_PARITY_ODD;
		else
			port_config.eParity = PC_PARITY_EVEN;
	} 
	else
		port_config.eParity = PC_PARITY_NONE;
	port_config.dwValidFileds |= PC_PARITY;

	switch((cflag & CSIZE)){
		case CS8:
			port_config.eDataBits = PC_DATABITS_8;
			break;
		default:
			port_config.eDataBits = PC_DATABITS_7;
			break;
	}
	port_config.dwValidFileds |= PC_DATA_BITS;

	if (cflag & CRTSCTS) {
		port_config.fCTS = TRUE;
		port_config.fRTS = TRUE;
	}
	port_config.dwValidFileds |= PC_CTS | PC_RTS;
	cnxt_control(inst, COMCTRL_CONTROL_PORTCONFIG, &port_config);
}
#else
static void
#ifdef FOUND_KTERMIOS
cnxt_set_termios(struct uart_port *port, struct ktermios *termios, struct ktermios *old)
#else
cnxt_set_termios(struct uart_port *port, struct termios *termios, struct termios *old)
#endif
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];
	PORT_CONFIG port_config={0};
	
	port_config.dwDteSpeed = uart_get_baud_rate(port, termios, old,port->uartclk / 16 / 0xffff,  port->uartclk / 4);
	port_config.dwValidFileds |= PC_DTE_SPEED;
			
	termios->c_cflag &= ~CMSPAR;
	
	if(termios->c_cflag & PARENB) {//0x400
		if(termios->c_cflag & PARODD)//0x1000
			port_config.eParity = PC_PARITY_ODD;
		else
			port_config.eParity = PC_PARITY_EVEN;//pari
	} 
	else
		port_config.eParity = PC_PARITY_NONE;
	port_config.dwValidFileds |= PC_PARITY;

	switch((termios->c_cflag & CSIZE)){
		case CS8:
			port_config.eDataBits = PC_DATABITS_8;
			break;
		default:
			port_config.eDataBits = PC_DATABITS_7;
			break;
	}	
	port_config.dwValidFileds |= PC_DATA_BITS;

	if (termios->c_cflag & CRTSCTS) {
		port_config.fCTS = TRUE;
		port_config.fRTS = TRUE;
	}
	port_config.dwValidFileds |= PC_CTS | PC_RTS;

	//cnxt_control(inst, COMCTRL_CONTROL_PORTCONFIG, &port_config);
	
	//printk(KERN_DEBUG "%s %d %08X\n", __FUNCTION__,port_config.dwDteSpeed,termios->c_cflag);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
	uart_update_timeout(port, termios->c_cflag, port_config.dwDteSpeed);
#endif
}
#endif

static void cnxt_enable_ms(struct uart_port *port)
{
	printk(KERN_DEBUG "%s\n", __FUNCTION__);
}

static void cnxt_release_port(struct uart_port *port)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];

	//printk(KERN_DEBUG "%s\n", __FUNCTION__);
	if(inst->port != port)
		printk(KERN_ERR"%s: inst->port(%p) != port(%p), i=%d cnxt_ports=%p\n", __FUNCTION__, inst->port, port, (int)(port - cnxt_ports), cnxt_ports);
	inst->port = NULL;
}

static int cnxt_request_port(struct uart_port *port)
{
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];

	//printk(KERN_DEBUG "%s\n", __FUNCTION__);
	if(!inst->port)
		inst->port = port;
	else {
		if(inst->port != port)
			printk(KERN_ERR"%s: inst->port(%p) != port(%p), i=%d cnxt_ports=%p\n", __FUNCTION__, inst->port, port, (int)(port - cnxt_ports), cnxt_ports);
	}
	return 0;
}

#ifndef PORT_CNXT
#define PORT_CNXT 36
#endif

static void cnxt_config_port(struct uart_port *port, int flags)
{
	//printk(KERN_DEBUG "%s\n", __FUNCTION__);
	if (flags & UART_CONFIG_TYPE && cnxt_request_port(port) == 0)
		port->type = PORT_CNXT;
}

/*
* Verify the new serial_struct (for TIOCSSERIAL).
* The only change we allow are to the flags
*/
static int cnxt_verify_port(struct uart_port *port, struct serial_struct *ser)
{
	int ret = 0;
	
	printk(KERN_DEBUG "%s %d\n", __FUNCTION__,ser->type);
	
	if (ser->type != PORT_CNXT)
		ret = -EINVAL;
	if (port->irq != ser->irq)
		ret = -EINVAL;
	/*if (ser->io_type != SERIAL_IO_MEM)
		ret = -EINVAL;*/
	if (port->uartclk / 16 != ser->baud_base)
		ret = -EINVAL;
	/*if ((void *)port->mapbase != ser->iomem_base)
		ret = -EINVAL;*/
	if (port->iobase != ser->port)
		ret = -EINVAL;
	if (ser->hub6 != 0)
		ret = -EINVAL;
	return ret;
}

static const char *cnxt_type(struct uart_port *port)
{
	return cnxt_serial_inst[port->line].typestr ? cnxt_serial_inst[port->line].typestr : CNXTDRVDSC;
}

static int cnxt_ioctl_port(struct uart_port *port, unsigned int cmd, unsigned long arg){
	int ret = -ENOIOCTLCMD;
	struct cnxt_serial_inst *inst = &cnxt_serial_inst[(port - cnxt_ports) / sizeof(struct uart_port)];
	ioctl_arg_t args;
	
	//printk(KERN_DEBUG "%s %lu\n", __FUNCTION__,cmd);
	switch(cmd){
		default:
			break;
		case CXT_USERSIGNAL:
			if (copy_from_user(&args, (ioctl_arg_t *)arg, sizeof(ioctl_arg_t))) 
				return -EACCES;
			inst->signal.user_pid = args.pid;
			ret = 0;
			break;
	}
	return ret;
}

static struct uart_ops cnxt_pops = {
	.tx_empty = cnxt_tx_empty,
	.set_mctrl = cnxt_set_mctrl,
	.get_mctrl =	cnxt_get_mctrl,
	.stop_tx =	cnxt_stop_tx,
	.start_tx	=	cnxt_start_tx,
	.stop_rx	=	cnxt_stop_rx,
	.enable_ms =	cnxt_enable_ms,
	.break_ctl	=	cnxt_break_ctl,
	.startup	=	cnxt_startup,
	.shutdown	=	cnxt_shutdown,
	.type = cnxt_type,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	.change_speed	=	cnxt_change_speed,
#else
	.set_termios	=	cnxt_set_termios,
#endif
	.release_port	=	cnxt_release_port,
	.request_port = cnxt_request_port,
	.config_port = cnxt_config_port,
	.verify_port	=	cnxt_verify_port,
	.ioctl = cnxt_ioctl_port
};

static int serialmajor = CNXTSERIALMAJOR;
#ifdef FOUND_MODULE_PARAM
module_param(serialmajor, int, 0);
#else
MODULE_PARM(serialmajor, "i");
#endif
MODULE_PARM_DESC(serialmajor, "Major device number for serial device");
#ifdef CNXTCALOUTMAJOR
static int calloutmajor = CNXTCALOUTMAJOR;
#ifdef FOUND_MODULE_PARAM
module_param(calloutmajor, int, 0);
#else
MODULE_PARM(calloutmajor, "i");
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
MODULE_PARM_DESC(calloutmajor, "Major device number for callout device");
#else
MODULE_PARM_DESC(calloutmajor, "Major device number for callout device (ignored/deprecated)");
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0) */
#endif

#ifdef COMCTRL_MONITOR_POUND_UG_SUPPORT
#ifdef FOUND_MODULE_PARAM
module_param(loglastcallstatus, int, 0);
#else
MODULE_PARM(loglastcallstatus, "i");
#endif
MODULE_PARM_DESC(loglastcallstatus, "Log AT#UG command output after each connection");
#endif

#ifdef CONFIG_PROC_FS

#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0) )
	//fixme inst-devnode
	static int cnxt_get_hwinst(struct seq_file *s, void *data)
	{
		struct cnxt_serial_inst *inst = &cnxt_serial_inst[cnxt_serial_shared_memory->hwInstNum];		
		seq_printf(s, "%d-%s\n", inst->devnode->hwInstNum, inst->devnode->hwInstName);
		return 0;
	}
	
	static int cnxt_proc_open_hwinst(struct inode *inode, struct file *filp)
	{
		return single_open(filp, cnxt_get_hwinst, NULL);
	}
	
	static int cnxt_get_hwprofile(struct seq_file *s, void *data)
	{
		struct cnxt_serial_inst *inst = &cnxt_serial_inst[cnxt_serial_shared_memory->hwInstNum];		

		seq_printf(s, "%s\n", inst->devnode->hwProfile);
		return 0;
	}
	

	static int cnxt_proc_open_hwprofile(struct inode *inode, struct file *filp)
	{
		return single_open(filp, cnxt_get_hwprofile, NULL);
	}
	
	static int cnxt_get_hwrevision(struct seq_file *s, void *data)
	{
		struct cnxt_serial_inst *inst = &cnxt_serial_inst[cnxt_serial_shared_memory->hwInstNum];		

		seq_printf(s, "%s\n", inst->devnode->hwRevision);
		return 0;
	}
	
	static int cnxt_proc_open_hwrevision(struct inode *inode, struct file *filp)
	{
		return single_open(filp, cnxt_get_hwrevision, NULL);
	}

	static int cnxt_get_lastcallstatus(struct seq_file *s, void *data){
		struct cnxt_serial_inst *inst = &cnxt_serial_inst[cnxt_serial_shared_memory->hwInstNum];		
		PORT_MONITOR_DATA monitorData;
		int len = PAGE_SIZE;
		char *page;
		
		page = cnxt_serial_shared_memory->buf;
		monitorData.dwSize = PAGE_SIZE;
		monitorData.pBuf = page;

		if (cnxt_monitor(inst, COMCTRL_MONITOR_POUND_UG, &monitorData) != COM_STATUS_SUCCESS)
			page[0] = '\0';
		else
			page[len-1] = '\0';
		seq_printf(s, "%s\n", page);
		return 0;
	}
	
	static int cnxt_proc_open_lastcallstatus(struct inode *inode, struct file *filp)
	{
		return single_open(filp, cnxt_get_lastcallstatus, NULL);
	}
	
	
	static const struct file_operations cnxt_proc_ops_hwinst = {
		.owner = THIS_MODULE,
		.open = cnxt_proc_open_hwinst,
		.llseek = seq_lseek,
		.read = seq_read,
		.release = single_release,
	};

	static const struct file_operations cnxt_proc_ops_hwprofile = {
		.owner = THIS_MODULE,
		.open = cnxt_proc_open_hwprofile,
		.llseek = seq_lseek,
		.read = seq_read,
		.release = single_release,		
	};
	
	static const struct file_operations cnxt_proc_ops_hwrevision = {
		.owner = THIS_MODULE,
		.open = cnxt_proc_open_hwrevision,
		.llseek = seq_lseek,
		.read = seq_read,
		.release = single_release,		
	};

	static const struct file_operations cnxt_proc_ops_lastcallstatus = {
		.owner = THIS_MODULE,
		.open = cnxt_proc_open_lastcallstatus,
		.llseek = seq_lseek,
		.read = seq_read,
		.release = single_release,		
	};
	
#else
	static int cnxt_get_hwinst(char *buf, char **start, off_t offset, int length, int *eof, void *data)
	{
		struct cnxt_serial_inst *inst = (struct cnxt_serial_inst *)data;
	
		if(offset)
			return 0;
	
		if(length > PAGE_SIZE)
			length = PAGE_SIZE;
	
		snprintf(buf, length - 1, "%d-%s\n", inst->devnode->hwInstNum, inst->devnode->hwInstName);
		buf[length-1] = '\0';
		return strlen(buf);
	}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
static ssize_t cnxt_flush_nvm(struct file *file, const char __user *buffer, size_t count, loff_t *pos)
#else
static int cnxt_flush_nvm(struct file *file, const char __user *buffer, unsigned long count, void *data)
#endif
{
	printk(KERN_DEBUG "%s: called\n", __FUNCTION__);

	NVM_WriteFlushList(TRUE);

	return count;
}

#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0) )
static const struct file_operations cnxt_proc_ops_flushnvm = {
	.owner = THIS_MODULE,
	.write = cnxt_flush_nvm
};
// TODO

#else
static int cnxt_get_hwprofile(char *buf, char **start, off_t offset, int length, int *eof, void *data)
{
	struct cnxt_serial_inst *inst = (struct cnxt_serial_inst *)data;

	if(offset)
	return 0;

	if(length > PAGE_SIZE)
	length = PAGE_SIZE;

	snprintf(buf, length - 1, "%s\n", inst->devnode->hwProfile);
	buf[length-1] = '\0';
	return strlen(buf);
}

static int cnxt_get_hwrevision(char *buf, char **start, off_t offset, int length, int *eof, void *data)
{
	struct cnxt_serial_inst *inst = (struct cnxt_serial_inst *)data;
	if(offset)
		return 0;
	if(length > PAGE_SIZE)
		length = PAGE_SIZE;
	snprintf(buf, length - 1, "%s\n", inst->devnode->hwRevision);
	buf[length-1] = '\0';
	return strlen(buf);
}

#ifdef COMCTRL_MONITOR_POUND_UG_SUPPORT
static int cnxt_get_lastcallstatus(char *page, char **start, off_t offset, int length, int *eof, void *data)
{
	struct cnxt_serial_inst *inst = (struct cnxt_serial_inst *)data;
	PORT_MONITOR_DATA monitorData;
	int len = PAGE_SIZE;

	monitorData.dwSize = len;
	monitorData.pBuf = page;

	if (cnxt_monitor(inst, COMCTRL_MONITOR_POUND_UG, &monitorData) != COM_STATUS_SUCCESS)
		page[0] = '\0';
	else
		page[len-1] = '\0';

	len = strlen(page);
	if (len <= offset+length)
		*eof = 1;
	*start = page + offset;
	len -= offset;
	if (len > length)
		len = length;
	if (len < 0)
		len = 0;
	return len;
}
#endif /* COMCTRL_MONITOR_POUND_UG_SUPPORT */

#endif

#endif /* CONFIG_PROC_FS */


#ifndef FOUND_UART_REGISTER_PORT

/**
*	uart_register_port - register a serial port
*      @drv: UART driver
*	@port: serial port template
*
*	Configure the serial port specified by the request.
*
*	The port is then probed and if necessary the IRQ is autodetected
*	If this fails an error is returned.
*
*	On success the port is ready to use and the line number is returned.
*/
static int uart_register_port(struct uart_driver *drv, struct uart_port *port)
{
	int i;
	struct uart_port *uart_port;
	int ret = -ENOSPC;

	down(&cnxt_port_sem);
	for (i = 0; i < NR_PORTS; i++) {
		uart_port = &cnxt_ports[i];
		if (uart_port->type == PORT_UNKNOWN)
			break;
	}
	if (i < NR_PORTS) {
		uart_remove_one_port(drv, uart_port);
		
		uart_port->iobase = port->iobase;
		uart_port->membase = port->membase;
		uart_port->irq      = port->irq;
		uart_port->uartclk  = port->uartclk;
		uart_port->iotype   = port->iotype;
		uart_port->flags    = port->flags | UPF_BOOT_AUTOCONF|UPF_SHARE_IRQ;
		uart_port->mapbase  = port->mapbase;
		//uart_port->iotype	= UPIO_PORT;
		if (port->dev)
			uart_port->dev = port->dev;
		ret = uart_add_one_port(drv, uart_port);
		if (ret == 0)
			ret = uart_port->line;
	}
	up(&cnxt_port_sem);
	return ret;
}

/**
*	uart_unregister_port - remove a serial port at runtime
*      @drv: UART driver
*	@line: serial line number
*
*	Remove one serial port.  This may not be called from interrupt
*	context.  We hand the port back to the our control.
*/
static void uart_unregister_port(struct uart_driver *drv, int line){
	struct uart_port *uart_port = &cnxt_ports[line];

	down(&cnxt_port_sem);
	uart_remove_one_port(drv, uart_port);
	uart_port->flags = 0;
	uart_port->type = PORT_UNKNOWN;
	uart_port->iobase = 0;
	uart_port->mapbase = 0;
	uart_port->membase = 0;
	uart_port->dev = NULL;
	uart_add_one_port(drv, uart_port);
	up(&cnxt_port_sem);
}
#endif /* FOUND_UART_REGISTER_PORT */


int cnxt_serial_add(POS_DEVNODE devnode, unsigned int iobase, void *membase, unsigned int irq, struct module *owner)
{
	struct cnxt_serial_inst *inst = NULL;
	int i, r;
	struct uart_port port;
	PORT_EVENT_HANDLER EvtHandler;
	unsigned long flags;

	if(!devnode)
		return -EINVAL;

	for(i = 0; i < NR_PORTS; i++) {
		spin_lock_irqsave(&cnxt_serial_inst[i].lock, flags);
		if(!cnxt_serial_inst[i].devnode) {
			inst = &cnxt_serial_inst[i];
			inst->devnode = devnode;
			spin_unlock_irqrestore(&cnxt_serial_inst[i].lock, flags);
			break;
		}
		spin_unlock_irqrestore(&cnxt_serial_inst[i].lock, flags);
	}

	if(!inst)
		return -ENOSPC;

	devnode->hwInstNum = i;
	cnxt_serial_shared_memory->hwInstNum=i;
	
	inst->hcomctrl = ComCtrl_Create();
	if(!inst->hcomctrl) {
		printk(KERN_DEBUG "%s: ComCtrlCreate failed!\n", __FUNCTION__);
		r = -EIO;
		goto errout;
	}
	devnode->hcomctrl = inst->hcomctrl;
			
	inst->port = NULL;
	inst->owner = owner;
	inst->mctrl_flags = 0;

	inst->typestr = kmalloc(strlen(CNXTDRVDSC) + strlen(devnode->hwInstName) + 5, GFP_KERNEL);
	if(inst->typestr)
		sprintf(inst->typestr, "%s (%s)", CNXTDRVDSC, devnode->hwInstName);

	if ((r=ComCtrl_Configure(inst->hcomctrl, COMCTRL_CONFIG_DEVICE_ID, devnode))) {
		printk(KERN_DEBUG "%s: ComCtrlConfigure DEVICE_ID failed (%d)\n", __FUNCTION__, r);
		r = -EIO;
		goto errout;
	}
	
	inst->evt_rxchar = 0;
	inst->evt_rxbreak = 0;
	inst->evt_rxovrn = 0;
	inst->evt_txempty = 1;
	inst->rxenabled = 0;
	inst->txenabled = 0;
	inst->readcount = inst->readoffset = 0;
//	inst->irq = irq;
	
	OsThreadScheduleInit(&inst->intr_tqueue, cnxt_intr, inst);

	EvtHandler.pfnCallback = (
#if (__GNUC__ == 3 && __GNUC_MINOR__ > 1) || __GNUC__ > 3
	__shimcall__
#endif
	void (*) (PVOID pRef, UINT32 dwEventMask)) cnxt_event_handler;
	
	EvtHandler.pRef = inst;
	
	if ((r=ComCtrl_Configure(inst->hcomctrl, COMCTRL_CONFIG_EVENT_HANDLER, &EvtHandler))) {
		printk(KERN_DEBUG "%s: ComCtrlConfigure EVENT_HANDLER failed (%d)\n", __FUNCTION__, r);
		r = -EIO;
		goto errout;
	}
	

	if((r=ComCtrl_Open(inst->hcomctrl))) {
		printk(KERN_ERR "%s: ComCtrlOpen failed (%d)\n", __FUNCTION__, r);
		r = -EIO;
		goto errout;
	}
		
	inst->mctrl_flags |= TIOCM_DSR;
		
	memset(&port, 0, sizeof(port));
	port.iobase = iobase;	
	port.membase = membase;
	port.irq = irq;
	
	port.membase	= (void __iomem *)~0;
	port.iobase = 0;
	port.irq = 0;	
	
	port.iotype = port.iobase ? SERIAL_IO_PORT : SERIAL_IO_MEM;
	port.uartclk = BASE_BAUD * 16;
	port.fifosize = 16;
	port.ops = &cnxt_pops;
	port.flags = ASYNC_BOOT_AUTOCONF;
	
	if((r = uart_register_port(&cnxt_reg, &port)) < 0) {
		inst->mctrl_flags &= ~TIOCM_DSR;
		ComCtrl_Close(inst->hcomctrl);
		goto errout;
	}

	if(r != i)
		printk(KERN_WARNING "%s: uart_register_port returned %d, expecting %d\n", __FUNCTION__, r, i);
	
	while(ComCtrl_Read(inst->hcomctrl, inst->readbuf, sizeof(inst->readbuf)) > 0);
	
#ifdef CONFIG_PROC_FS
	if(cnxt_serial_proc_dir) {
		char dirname[10];

		snprintf(dirname, sizeof(dirname), "%d", i);
		inst->proc_unit_dir = proc_mkdir(dirname, cnxt_serial_proc_dir);
		if(inst->proc_unit_dir) {
#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0) )
			inst->proc_hwinst = proc_create_data("hwinst", 0, inst->proc_unit_dir, &cnxt_proc_ops_hwinst, inst);
			inst->proc_hwprofile = proc_create_data("hwprofile", 0, inst->proc_unit_dir, &cnxt_proc_ops_hwprofile, inst);
			inst->proc_hwrevision = proc_create_data("hwrevision", 0, inst->proc_unit_dir, &cnxt_proc_ops_hwrevision, inst);
#ifdef COMCTRL_MONITOR_POUND_UG_SUPPORT
			inst->proc_lastcallstatus = proc_create_data("lastcallstatus", 0, inst->proc_unit_dir, &cnxt_proc_ops_lastcallstatus, inst);
#endif
#else
			inst->proc_hwinst = create_proc_read_entry("hwinst", 0, inst->proc_unit_dir, cnxt_get_hwinst, inst);
			inst->proc_hwprofile = create_proc_read_entry("hwprofile", 0, inst->proc_unit_dir, cnxt_get_hwprofile, inst);
			inst->proc_hwrevision = create_proc_read_entry("hwrevision", 0, inst->proc_unit_dir, cnxt_get_hwrevision, inst);
#ifdef COMCTRL_MONITOR_POUND_UG_SUPPORT
			inst->proc_lastcallstatus = create_proc_read_entry("lastcallstatus", 0, inst->proc_unit_dir, cnxt_get_lastcallstatus, inst);
#endif
#endif
		}
	}
#endif /* CONFIG_PROC_FS */

	return 0;

errout:
	if(inst->hcomctrl) {
		ComCtrl_Destroy(inst->hcomctrl);
		inst->hcomctrl = NULL;
	}

	spin_lock_irqsave(&inst->lock, flags);
	inst->devnode = NULL;
	spin_unlock_irqrestore(&inst->lock, flags);

	return r;
}

int cnxt_serial_remove(POS_DEVNODE devnode)
{
	struct cnxt_serial_inst *inst = NULL;
	int i;
	unsigned long flags;

	if(!devnode)
		return -EINVAL;

	for(i = 0; i < NR_PORTS; i++) {
		spin_lock_irqsave(&cnxt_serial_inst[i].lock, flags);
		if(cnxt_serial_inst[i].devnode == devnode) {
			inst = &cnxt_serial_inst[i];
			spin_unlock_irqrestore(&cnxt_serial_inst[i].lock, flags);
			break;
		}
		spin_unlock_irqrestore(&cnxt_serial_inst[i].lock, flags);
	}

	if(!inst)
		return -EINVAL;

	inst->mctrl_flags &= ~TIOCM_DSR;

#ifdef CONFIG_PROC_FS
#ifdef COMCTRL_MONITOR_POUND_UG_SUPPORT
	if(inst->proc_lastcallstatus) {
		remove_proc_entry("lastcallstatus", inst->proc_unit_dir);
	}
#endif
	if(inst->proc_hwrevision) {
		remove_proc_entry("hwrevision", inst->proc_unit_dir);
	}
	if(inst->proc_hwprofile) {
		remove_proc_entry("hwprofile", inst->proc_unit_dir);
	}
	if(inst->proc_hwinst) {
		remove_proc_entry("hwinst", inst->proc_unit_dir);
	}
	if(inst->proc_unit_dir) {
		char dirname[10];

		snprintf(dirname, sizeof(dirname), "%d", i);
		remove_proc_entry(dirname, cnxt_serial_proc_dir);
	}
#endif /* CONFIG_PROC_FS */

	uart_unregister_port(&cnxt_reg, i);

	if(inst->hcomctrl) {
		devnode->hcomctrl = NULL;
		ComCtrl_Close(inst->hcomctrl);
		ComCtrl_Destroy(inst->hcomctrl);
		inst->hcomctrl = NULL;
	}

	if(inst->typestr) {
		kfree(inst->typestr);
		inst->typestr = NULL;
	}

	spin_lock_irqsave(&inst->lock, flags);
	inst->devnode = NULL;
	spin_unlock_irqrestore(&inst->lock, flags);

	return 0;
}

static int __init cnxt_serial_init(void)
{
	int i, ret;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	cnxt_reg.normal_major = serialmajor;
	cnxt_reg.callout_major = calloutmajor;
#else
	cnxt_reg.major = serialmajor;
	(void)calloutmajor;
#endif

	if(serialmajor == 0) {
		printk(KERN_ERR "%s: serialmajor parameter must be non-null\n", __FUNCTION__);
		return -EINVAL;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	if(calloutmajor == 0) {
		printk(KERN_ERR "%s: calloutmajor parameter must be non-null\n", __FUNCTION__);
		return -EINVAL;
	}

	if(serialmajor == calloutmajor) {
		printk(KERN_ERR "%s: serialmajor and calloutmajor parameter values must differ\n", __FUNCTION__);
		return -EINVAL;
	}
#endif

	if ((ret = uart_init()) != 0)
		return ret;

	//fixme add new memory allocation ring 0 memory
	i=(sizeof(struct cnxt_serial_inst) + sizeof(struct uart_port)) * NR_PORTS + sizeof(struct _cnxt_serial_shared_memory) + PAGE_SIZE*2;
	cnxt_serial_inst = (struct cnxt_serial_inst *)kmalloc(i,GFP_KERNEL);
	if(cnxt_serial_inst == NULL){
		printk(KERN_ERR "%s: bad allocation cnxt_serial_inst \n", __FUNCTION__);
		return -EINVAL;
	}
	memset(cnxt_serial_inst,0,i);
	cnxt_ports = (struct uart_port *)&cnxt_serial_inst[NR_PORTS];
	cnxt_serial_shared_memory = (struct _cnxt_serial_shared_memory *)&cnxt_ports[NR_PORTS];
	cnxt_serial_shared_memory->buf = (unsigned char *)(cnxt_serial_shared_memory+1);
	
	for(i = 0; i < NR_PORTS; i++) {
		spin_lock_init(&cnxt_serial_inst[i].lock);
		cnxt_ports[i].ops = &cnxt_pops; /* uart_register_port() might not set ops */
		cnxt_ports[i].line = i;
	}

#ifdef CONFIG_PROC_FS
	cnxt_serial_proc_dir = proc_mkdir(PROC_PREFIX CNXTTARGET, proc_root_driver);

	if (cnxt_serial_proc_dir) {
#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0) )
		cnxt_serial_flush_nvm = proc_create("flush_nvm", 0, cnxt_serial_proc_dir, &cnxt_proc_ops_flushnvm);
#else
		cnxt_serial_flush_nvm = create_proc_entry("flush_nvm", 0, cnxt_serial_proc_dir);
		if (cnxt_serial_flush_nvm)
			cnxt_serial_flush_nvm->write_proc = cnxt_flush_nvm;
#endif
	}
#endif

	ret = uart_register_driver(&cnxt_reg);
	if(ret) {
#ifdef CONFIG_PROC_FS
		if(cnxt_serial_proc_dir) {
			if (cnxt_serial_flush_nvm)
				remove_proc_entry("flush_nvm", cnxt_serial_proc_dir);
			remove_proc_entry(PROC_PREFIX CNXTTARGET, proc_root_driver);
		}
#endif
		uart_exit();
		return ret;
	}
	
#if !(LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
	for(i = 0; i < NR_PORTS; i++)
		uart_add_one_port(&cnxt_reg, &cnxt_ports[i]);
#endif
	return ret;
}

static void __exit cnxt_serial_exit(void)
{
#if !(LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
	int i;

	for(i = 0; i < NR_PORTS; i++)
		uart_remove_one_port(&cnxt_reg, &cnxt_ports[i]);
#endif
	uart_unregister_driver(&cnxt_reg);
#ifdef CONFIG_PROC_FS
	if(cnxt_serial_proc_dir) {
		if (cnxt_serial_flush_nvm)
			remove_proc_entry("flush_nvm", cnxt_serial_proc_dir);
		remove_proc_entry(PROC_PREFIX CNXTTARGET, proc_root_driver);
	}
#endif	
	uart_exit();
	if(cnxt_serial_inst != NULL)
		kfree(cnxt_serial_inst);
	cnxt_serial_inst = NULL;
	cnxt_ports = NULL;
	cnxt_serial_shared_memory = NULL;
}

module_init(cnxt_serial_init);
module_exit(cnxt_serial_exit);

EXPORT_SYMBOL_NOVERS(cnxt_serial_add);
EXPORT_SYMBOL_NOVERS(cnxt_serial_remove);

MODULE_AUTHOR("Copyright (C) 2003-2004 Linuxant inc.");
MODULE_DESCRIPTION("Virtual serial port driver for Conexant modems");
MODULE_LICENSE("GPL\0for files in the \"GPL\" directory; for others, only LICENSE file applies");
MODULE_INFO(supported, "yes");

#ifdef CNXTSERIAL_INCLUDE_CORE

#undef EXPORT_SYMBOL
#define EXPORT_SYMBOL(x)

#undef MODULE_DESCRIPTION
#define MODULE_DESCRIPTION(x)

#undef MODULE_LICENSE
#define MODULE_LICENSE(x)

#undef MODULE_INFO
#define MODULE_INFO(x,y)

#undef module_init
#define module_init(x)

#undef module_exit
#define module_exit(x)

#include "serial_core.c"
#endif

