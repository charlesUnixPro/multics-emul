/*
    iom.c
        See: Document 43A239854 -- 6000B I/O Multiplexer --  43A239854_600B_IOM_Spec_Jul75.pdf
        See also: http://www.multicians.org/fjcc5.html -- Communications and Input/Output Switching in a Multiplex Computing System
        See also: Patents: 4092715, 4173783, 1593312

    3.10 some more physical switches

    Config switch: 3 positions -- Standard GCOS, Extended GCOS, Multics
*/

#include "hw6180.h"
#include <sys/time.h>

#define IOM_A_MBX 01400     /* location of mailboxes for IOM A */
#define IOM_CONNECT_CHAN 2

extern t_uint64 M[];    /* memory */
extern cpu_ports_t cpu_ports;
extern scu_t scu;   // BUG: we'll need more than one for max memory.  Unless we can abstract past the physical HW's capabilities
extern iom_t iom;

enum iom_sys_faults {
    // from 4.5.1; descr from AN87, 3-9
    iom_no_fault = 0,
    iom_ill_chan = 01,      // PCW to chan with chan number >= 40
    // iom_ill_ser_req=02,  // chan requested svc with a service code of zero or bad chan #
    iom_256K_of=04,         // 256K overflow -- address decremented to zero, but not tally
    iom_lpw_tro_conn = 05,  // tally was zero for an update LPW (LPW bit 21==0) when the LPW was fetched for the connect channel
    iom_not_pcw_conn=06,    // DCW for conn channel had bits 18..20 != 111b
    // iom_cp1_data=07,     // DCW was TDCW or had bits 18..20 == 111b
    iom_ill_tly_cont = 013, // LPW bits 21-22 == 00 when LPW was fetched for the connect channel
    // 14 LPW had bit 23 on in Multics mode
};
enum iom_user_faults {  // aka central status
    // from 4.5.2
    iom_lpw_tro = 1,    //  tally was zero for an update LPW (LPW bit 21==0) when the LPW was fetched  and TRO-signal (bit 22) is on
    iom_bndy_vio = 03,
};

typedef struct {
    uint32 dcw; // bits 0..17
    flag_t ires;    // bit 18; IDCW restrict
    flag_t hrel;    // bit 19; hardware relative addressing
    flag_t ae;      // bit 20; address extension
    flag_t nc;      // bit 21; no tally; zero means update tally
    flag_t trunout; // bit 22; signal tally runout?
    flag_t srel;    // bit 23; software relative addressing; not for Multics!
    uint32 tally;   // bits 24..35
    // following not valid for paged mode; see B15; but maybe IOM-B non existant
    uint32 lbnd;
    uint32 size;
    uint32 idcw;    // ptr to most recent dcw, idcw, ...
} lpw_t;

// Much of this is from, AN87 as 43A23985 lacked details of 0..18 and 22..36
typedef struct pcw_s {
    int dev_cmd;    // 6 bits; 0..5
    int dev_code;   // 6 bits; 6..11
    int ext;        // 6 bits; 12..17
    int cp;         // 3 bits; 18..20, must be all ones
    flag_t mask;    // 1 bit; bit 21
    int control;    // 2 bits; bit 22..23
    int chan_cmd;   // 6 bits; bit 24..29
    int chan_data;  // 6 bits; bit 30..35
    //
    int chan;       // 6 bits; bits 3..8 of word 2
} pcw_t;

#if 0
// from AN87, 3-8
typedef struct {
    int channel;    // 9 bits
    int serv_req;   // 5 bits; see AN87, 3-9
    int ctlr_fault; // 4 bits; SC ill action codes, AN87 sect II
    int io_fault;   // 6 bits; see enum iom_sys_faults
} sys_fault_t;
#endif

// from AN87, 3-11
typedef struct {
    int chan;       // not part of the status word; simulator only
    int major;
    int substatus;
    // even/odd bit
    // status marker bit
    // soft, 2 bits set to zero by hw
    // initiate bit
    // chan_stat; 3 bits; 1=busy, 2=invalid chan, 3=incorrect dcw, 4=incomplete
    // iom_stat; 3 bits; 1=tro, 2=2tdcw, 3=bndry, 4=addr ext, 5=idcw,
    int addr_ext;   // BUG: not maintained
    // rcound;  // residue in PCW or last IDCW
    // addr;    // addr of *next* data word to be transmitted
    // char_pos
    // read;    // flag
    // type;    // 1 bit
    // dcw_residue; // residue in tally of last dcw
    flag_t power_off;
} chan_status_t;

// #define MAXCHAN 64
static chan_status_t chan_status;


static void dump_iom(void);
static void dump_iom_mbx(int base, int i);
static void iom_fault(int chan, int src_line, int is_sys, int signal);
static int list_service(int chan, int first_list, int *ptro, int *addr);
static int handle_pcw(int chan, int addr);
static int do_channel(int chan, pcw_t *p);
static int do_dcw(int chan, int addr, int *control, int *hack);
static int lpw_write(int chan, int chanloc, const lpw_t* lpw);
static int do_conn_chan(void);
static char* lpw2text(const lpw_t *p, int conn);
static char* pcw2text(const pcw_t *p);
static int parse_lpw(lpw_t *p, int addr, int is_conn);
static int parse_pcw(pcw_t *p, int addr, int ext);
static int dev_send_pcw(int chan, pcw_t *p);
static int status_service(int chan);
static int send_chan_flags();

void iom_interrupt()
{
    // Simulate receipt of a $CON signal (caused by CPU using a CIOC
    // instruction to signal SCU)
    // Actually, the BUS would give us more than just the channel:
    //      for program interrupt service
    //          interrupt level
    //          addr ext (0-2)
    //          channel number
    //          servicer request code
    //          addr ext (3-5)
    //      for status service signals
    //          ...
    //      for data or list service
    //          direct data addr
    //          addr ext (0-2)
    //          channel number
    //          service request code
    //          mode
    //          DP
    //          chan size
    //          addr ext (3-5)

    // dump_iom();

    debug_msg("IOM::CIOC", "Starting\n");
    do_conn_chan();
    debug_msg("IOM::CIOC", "Finished\n");
}

static int do_conn_chan()
{
    
    // BUG: don't detect channel status #1 "unexpected PCW (connect while busy)"

    int ptro = 0;   // pre-tally-run-out, e.g. end of list
    int addr;
    while (ptro == 0) {
        debug_msg("IOM::CIOC", "Doing list service for Connect Channel\n");
        int ret = list_service(IOM_CONNECT_CHAN, 1, &ptro, &addr);
        if (! ret) {
            // Do next PCW  (0720201 at loc zero for bootload_tape_label.alm)
            ret = handle_pcw(IOM_CONNECT_CHAN, addr);
        }
        // BUG: update LPW for chan 2 in core -- unless list service does it
        // BUG: Stop if tro system fault occured
    }
}


static int handle_pcw(int chan, int addr)
{
    // Only called by the connect channel

    debug_msg("IOM::connect-chan", "PCW for chan %d, addr 0%o\n", chan, addr);
    pcw_t pcw;
    parse_pcw(&pcw, addr, 1);
    debug_msg("IOM::connect-chan", "PCW is: %s\n", pcw2text(&pcw));

    if (pcw.chan < 0 || pcw.chan >= 040) {  // 040 == 32 decimal
        iom_fault(chan, __LINE__, 1, iom_ill_chan); // BUG: what about ill-ser-req? -- is iom issuing a pcw or is channel requesting svc?
        return 1;
    }
    if (pcw.cp != 07) {
        iom_fault(chan, __LINE__, 1, iom_not_pcw_conn);
        return 1;
    }

    if (pcw.mask) {
        // BUG: set mask flags for channel?
        complain_msg("IOM::connect-chan", "PCW Mask not implemented\n");
        cancel_run(STOP_BUG);
        return 1;
    }
    return do_channel(pcw.chan, &pcw);
}


static int do_channel(int chan, pcw_t *p)
{

    int ret = 0;
    chan_status.chan = chan;

    debug_msg("IOM::do-chan", "Starting for channel %d\n", chan);

    // First, send any PCW command to the device

    ret = dev_send_pcw(chan, p);

    // Second, check if PCW tells use to request a list service, send an interrupt, etc
    // BUG: A loop for calling list service and doing returned DCWs should probably be here

    int control = p->control;
    int first_list = 1;
    while (ret == 0 && control != 0) {
        switch (control) {
            case 0:
                // do nothing, terminate
                break;
            case 2: {
                int addr;
                debug_msg("IOM::do-chan", "Asking for list service\n");
                if (list_service(chan, first_list, NULL, &addr) != 0) {
                    ret = 1;
                    warn_msg("IOM::do-chan", "List service indicates failure\n");
                } else {
                    debug_msg("IOM::do-chan", "List service yields DCW at addr 0%o\n", addr);
                    control = -1;
                    int hack = 0;
                    ret = do_dcw(chan, addr, &control, &hack);
                    debug_msg("IOM::do-chan", "Back from latest do_dcw (at %0o); control = %d\n", addr, control);
                    if (chan_status.major != 0)
                        control = 0;
                    else
                        if (hack && first_list) {
                            debug_msg("IOM::do-chan", "HACK: Asking for another list service (contrary to 43A239854)\n");
                            control = 2;
                        }
                }
                first_list = 0;
                break;
            }
            case 3:
                // BUG: set marker interrupt and proceed (list service)
                complain_msg("IOM::do-chan", "Set marker not implemented\n");
                cancel_run(STOP_BUG);
                ret = 1;
                break;
            default:
                complain_msg("IOM::do-chan", "Bad PCW control == %d\n", control);
                cancel_run(STOP_BUG);
                ret = 1;
        }
    }

    // Next: probably loop performing DCWs and asking for list service

    // BUG: skip status service if system fault exists
    debug_msg("IOM::do-chan", "Requesting Status service\n");
    status_service(chan);

    /* -- from orangesquid iom.c -- seems somewhat bogus
    read mbx at iom plus 4*pcw.chan
    take lpw from that loc; extract tally
    read scw
    extract addr
    for i = 0 .. tally; do-dcw(dcw+i)
    */

    debug_msg("IOM::do-chan", "Finished\n");
    return ret;
}


static int list_service_orig(int chan, int first_list, int *ptro)
{

    // Flowchare 256K overflow checks relate to 18bit offset & tally incr
    // TRO -- tally runout, a fault (not matching CPU fault names)
    //      user fault: lpw tr0 sent to channel (sys fault lpw tr0 for conn chan)
    // PTRO -- pre-tally runout -- only used internally?
    // page 82 etc for status

    // addr upper bits -- from: PCW in extended GCOS mode, or IDCW for list service (2.1.2)
    // paging mode given by PTP (page table ptr) in second word of PCW (for IOM-B)
    // 3 modes: std gcos; ext gcos, paged
    // paged: all acc either paged or extneded gcos, not relative moe

}


static int list_service(int chan, int first_list, int *ptro, int *addrp)
{
    // Returns core address of next PCW or DCW in *addrp

    lpw_t lpw;
    int chanloc = IOM_A_MBX + chan * 4;

    debug_msg("IOM::list-service", "Checking LPW for channel %0o(%d dec) at addr %0o\n", chan, chan, chanloc);

    *addrp = -1;
    parse_lpw(&lpw, chanloc, chan == IOM_CONNECT_CHAN);
    debug_msg("IOM::list-service", "LPW: %s\n", lpw2text(&lpw, chan == IOM_CONNECT_CHAN));

    if (lpw.srel) {
        complain_msg("IOM::list-service", "LPW with bit 23 on is invalid for Multics mode\n");
        iom_fault(chan, __LINE__, 1, 014);  // BUG, want enum
        cancel_run(STOP_BUG);
        return 1;
    }
    if (first_list) {
        lpw.hrel = lpw.srel;
    }
    if (lpw.ae != lpw.hrel) {
        warn_msg("IOM::list-service", "AE does not match HREL\n");
        cancel_run(STOP_BUG);
    }

    // Check for TRO or PTRO at time that LPW is fetched -- not later

    if (ptro != NULL)
        *ptro = 0;
    int addr = lpw.dcw;
    if (chan == IOM_CONNECT_CHAN) {
        if (lpw.nc == 0 && lpw.trunout == 0) {
            warn_msg("IOM::list-service", "Illegal tally connect channel\n");
            iom_fault(chan, __LINE__, 1, iom_ill_tly_cont);
            cancel_run(STOP_WARN);
            return 1;
        }
        if (lpw.nc == 0 && lpw.trunout == 1)
            if (lpw.tally == 0) {
                warn_msg("IOM::list-service", "TRO on connect channel\n");
                iom_fault(chan, __LINE__, 1, iom_lpw_tro_conn);
                cancel_run(STOP_WARN);
                return 1;
            }
        if (lpw.nc == 1) {
            // we're not updating tally, so pretend it's at zero
            if (ptro != NULL)
                *ptro = 1;  // forced, see pg 23
        }
        *addrp = addr;  // BUG: force y-pair
        // next: channel should pull PCW from core
    } else {
        // non connect channel
        // first, do an addr check for overflow
        int overflow = 0;
        if (lpw.ae) {
            int sz = lpw.size;
            if (lpw.size == 0) {
                warn_msg("IOM::list-sevice", "LPW size is zero\n");
                sz = 010000;    // 4096
            }
            if (addr >= sz)     // BUG: was >
                overflow = 1; // signal or record below
            else
                addr = lpw.lbnd + addr ;
        }
        // see flowchart 4.3.1b
        if (lpw.nc == 0 && lpw.trunout == 0) {
            if (overflow) {
                iom_fault(chan, __LINE__, 1, iom_256K_of);
                return 1;
            }
        }
        if (lpw.nc == 0 && lpw.trunout == 1) {
            // BUG: Chart not fully handled (nothing after (C) except T-DCW detect)
            for (;;) {
                if (lpw.tally == 0) {
                    warn_msg("IOW::list-service", "TRO on channel 0%o\n", chan);
                    iom_fault(chan, __LINE__, 0, iom_lpw_tro);
                    cancel_run(STOP_WARN);
                    // user fault, no return
                    break;
                }
                if (lpw.tally > 1) {
                    if (overflow) {
                        iom_fault(chan, __LINE__, 1, iom_256K_of);
                        return 1;
                    }
                }
                // Check for T-DCW
                int t = getbits36(M[addr], 18, 3);
                if (t == 2) {
                    complain_msg("IOW::list-service", "Transfer-DCW not implemented\n");
                    return 1;
                } else
                    break;
            }
        }
        *addrp = addr;
        // if in GCOS mode && lpw.ae) fault;    // bit 20
        // next: channel should pull DCW from core
    }

    int cp = getbits36(M[addr], 18, 3);
    if (cp == 7) {
        // BUG: update idcw fld of lpw
    }

    // int ret;

//-------------------------------------------------------------------------
// ALL THE FOLLOWING HANDLED BY PART "D" of figure 4.3.1b and 4.3.1c
//          if (pcw.chan == IOM_CONNECT_CHAN) {
//              debug_msg("IOM::pcw", "Connect channel does not return status.\n");
//              return ret;
//          }
//          // BUG: need to write status to channel (temp: todo chan==036)
// update LPW for chan (not 2)
// update DCWs as used
// SCW in mbx
// last, send an interrupt (still 3.0)
// However .. conn chan does not interrupt, store status, use dcw, or use scw
//-------------------------------------------------------------------------

    // Part "D" of 4.3.1c

    // BUG BUG ALL THE FOLLOWING IS BOTH CORRECT AND INCORRECT!!! Section 3.0 states
    // BUG BUG that LPW for CONN chan is updated in core after each chan is given PCW
    // BUG BUG Worse, below is prob for channels listed in dcw/pcw, not conn

    if (chan_status.chan != IOM_CONNECT_CHAN)
        send_chan_flags();

    int write_lpw = 0;
    int write_lpw_ext = 0;
    int write_any = 1;
    if (lpw.nc == 0) {
        if (lpw.trunout == 1)
            if (lpw.tally == 1)
                if (ptro != NULL)
                    *ptro = 1;
            else if (lpw.tally == 0) {
                write_any = 0;
                if (chan == IOM_CONNECT_CHAN)
                    iom_fault(chan, __LINE__, 1, iom_lpw_tro_conn);
                else
                    iom_fault(chan, __LINE__, 0, iom_bndy_vio); // BUG: might be wrong
            }
        if (write_any)
            -- lpw.tally;
            if (chan == IOM_CONNECT_CHAN)
                lpw.dcw += 2;   // pcw is two words
            else
                ++ lpw.dcw;     // dcw is one word
    } else  {
        // note: ptro forced earlier
        write_any = 0;
    }

int did_idcw = 0;   // BUG
int did_tdcw = 0;   // BUG
    if (lpw.nc == 0) {
        write_lpw = 1;
        if (did_idcw || first_list)
            write_lpw_ext = 1;
    } else {
        // no update
        if (did_idcw || first_list) {
            write_lpw = 1;
            write_lpw_ext = 1;
        } else if (did_tdcw) 
            write_lpw = 1;
    }
    //if (pcw.chan != IOM_CONNECT_CHAN) {
    //  ; // BUG: write lpw
    //}
    lpw_write(chan, chanloc, &lpw);     // BUG: we always write LPW

    debug_msg("IOM::list-sevice", "returning\n");
    return 0;   // BUG: unfinished
}


static int parse_pcw(pcw_t *p, int addr, int ext)
{
    p->dev_cmd = getbits36(M[addr], 0, 6);
    p->dev_code = getbits36(M[addr], 6, 6);
    p->ext = getbits36(M[addr], 12, 6);
    p->cp = getbits36(M[addr], 18, 3);
    p->mask = getbits36(M[addr], 21, 1);
    p->control = getbits36(M[addr], 22, 2);
    p->chan_cmd = getbits36(M[addr], 24, 6);
    p->chan_data = getbits36(M[addr], 30, 6);
    if (ext) {
        p->chan = getbits36(M[addr+1], 3, 6);
        uint x = getbits36(M[addr+1], 9, 27);
        if (x != 0) {
            // BUG: Should only check if in GCOS or EXT GCOS Mode
            complain_msg("IOM::pcw", "Page Table Pointer for model IOM-B detected\n");
            cancel_run(STOP_BUG);
        }
    } else {
        p->chan = -1;
    }
}


static char* pcw2text(const pcw_t *p)
{
    // WARNING: returns single static buffer
    static char buf[80];
    sprintf(buf, "[dev-cmd=0%o, dev-code=0%o, ext=0%o, mask=%d, ctrl=0%o, chan-cmd=0%o, chan-data=0%o, chan=0%o]",
        p->dev_cmd, p->dev_code, p->ext, p->mask, p->control, p->chan_cmd, p->chan_data, p->chan);
    return buf;
}



static int do_pcw(int chan, pcw_t *p)
{
    debug_msg("IOM::do-pcw", "\n");
    return dev_send_pcw(chan, p);
}

static int dev_send_pcw(int chan, pcw_t *p)
{
    debug_msg("IOM::dev-send-pcw", "\n");

    DEVICE* devp = iom.devices[chan];
    if (devp == NULL || devp->units == NULL) {
        // BUG: no device connected, what's the fault code(s) ?
        chan_status.power_off = 1;
        iom_fault(chan, __LINE__, 0, 0);
        cancel_run(STOP_WARN);
        return 1;
    }
    chan_status.power_off = 0;

    switch(iom.channels[chan]) {
        case DEV_NONE:
            // BUG: no device connected, what's the fault code(s) ?
            chan_status.power_off = 1;
            iom_fault(chan, __LINE__, 0, 0);
            cancel_run(STOP_WARN);
            return 1;
        case DEV_TAPE: {
            int ret = mt_iom_cmd(p->chan, p->dev_cmd, p->dev_code, &chan_status.major, &chan_status.substatus);
            debug_msg("IOM::dev-send-pcw", "MT returns major code 0%o substatus 0%o\n", chan_status.major, chan_status.substatus);
            return 0;   // ignore ret in favor of chan_status.{major,substatus}
        }
        default:
            complain_msg("IOM::dev-send-pcw", "Unknown device type 0%o\n", iom.channels[chan]);
            iom_fault(chan, __LINE__, 1, 0);    // BUG: need to pick a fault code
            cancel_run(STOP_BUG);
            return 1;
    }
    return -1;  // not reached
}


static int dev_io(int chan, t_uint64 *wordp)
{
    DEVICE* devp = iom.devices[chan];
    if (devp == NULL || devp->units == NULL) {
        // BUG: no device connected, what's the fault code(s) ?
        chan_status.power_off = 1;
        iom_fault(chan, __LINE__, 0, 0);
        cancel_run(STOP_WARN);
        return 1;
    }
    chan_status.power_off = 0;

    switch(iom.channels[chan]) {
        case DEV_NONE:
            // BUG: no device connected, what's the fault code(s) ?
            chan_status.power_off = 1;
            iom_fault(chan, __LINE__, 0, 0);
            cancel_run(STOP_WARN);
            return 1;
        case DEV_TAPE: {
            int ret = mt_iom_io(chan, wordp, &chan_status.major, &chan_status.substatus);
            if (ret != 0 || chan_status.major != 0)
                debug_msg("IOM::dev-io", "MT returns major code 0%o substatus 0%o\n", chan_status.major, chan_status.substatus);
            return 0;   // ignore ret in favor of chan_status.{major,substatus}
        }
        default:
            complain_msg("IOM::dev-io", "Unknown device type 0%o\n", iom.channels[chan]);
            iom_fault(chan, __LINE__, 1, 0);    // BUG: need to pick a fault code
            cancel_run(STOP_BUG);
            return 1;
    }
    return -1;  // not reached
}


static int do_ddcw(int chan, int addr, int *control)
{
    // IOTD, IOTP, IONTP

    uint daddr = getbits36(M[addr], 0, 18);
    uint cp = getbits36(M[addr], 18, 3);
    uint type = getbits36(M[addr], 22, 3);
    uint tally = getbits36(M[addr], 24, 12);
    debug_msg("IOW::DDCW", "Data DCW: %012Lo: addr=0%o, cp=0%o, tally=%d\n", M[addr], daddr, cp, tally);

    // impossible for (cp == 7); see do_dcw

    // AE, the upper 6 bits of data addr (bits 0..5) from LPW
    // if rel == 0; abs; else absolutize & boundry check
    // BUG: Need to munge daddr

    t_uint64 word = 0;
    t_uint64 *wordp = (type == 3) ? &word : M + daddr;  // 2 impossible; see do_dcw
    int ret;
    debug_msg("IOM::DDCW", "I/O Request(s) starting at addr 0%o\n", daddr);
    for (;;) {
        ret = dev_io(chan, wordp);
        if (ret != 0 || chan_status.major != 0)
            break;
        ++daddr;    // todo: remove from loop
        ++wordp;
        if (--tally <= 0)
            break;
    }
    debug_msg("IOM::DDCW", "Last I/O Request was to addr 0%o; tally now %d\n", daddr, tally);
    // set control ala PCW as method to indicate terminate or proceed
    if (type == 0)
        *control = 0;
    else
        *control = 3;   // 2 or 3?
    // update dcw
#if 0
    // Assume that DCW is only in scratchpad (bootload_tape_label.alm rd_tape reuses same DCW on each call)
    M[addr] = setbits36(M[addr], 0, 18, daddr);
    M[addr] = setbits36(M[addr], 24, 12, tally);
    debug_msg("IOM::DDCW", "Data DCW update: %012Lo: addr=%0o, tally=%d\n", M[addr], daddr, tally);
#endif
    return ret;
}


static int do_dcw(int chan, int addr, int *controlp, int *hack)
{
    debug_msg("IOM::dwc", "chan %d, addr 0%o\n", chan, addr);
    int cp = getbits36(M[addr], 18, 3);
    if (cp == 7) {
        // instr dcw
        int ec = getbits36(M[addr], 21, 1);
        if (ec) {
            // BUG: Check LPW bit 23
            // M[addr] = setbits36(M[addr], 12, 6, present_addr_extension);
            complain_msg("IOW::DCW", "I-DCW bit EC not implemented\n");
            cancel_run(STOP_BUG);
        }
        debug_msg("IOW::DCW", "I-DCW found\n");
        pcw_t pcw;
        parse_pcw(&pcw, addr, 0);
        pcw.mask = 0;   // EC not mask
        pcw.chan = chan;    // Real HW would not populate
        debug_msg("IOM::dcw", "I-DCW: %s\n", pcw2text(&pcw));
        *controlp = pcw.control;
        *hack = 1;  // do_pcw with either return non zero or will set chan_status.major
        return do_pcw(chan, &pcw);
    } else {
        int type = getbits36(M[addr], 22, 2);
        if (type == 2) {
            // transfer
            complain_msg("IOW::DCW", "Transfer-DCW not implemented\n");
            return 1;
        } else {
            // IOTD, IOTP, or IONTP -- i/o (non) transfer
            int ret = do_ddcw(chan, addr, controlp);
            return ret;
        }
    }
}

char* print_lpw(t_addr addr)
{
    lpw_t temp;
    int chan = (addr - IOM_A_MBX) / 4;
    parse_lpw(&temp, addr, chan == IOM_CONNECT_CHAN);
    static char buf[80];
    sprintf(buf, "Chan 0%o -- %s", chan, lpw2text(&temp, chan == IOM_CONNECT_CHAN));
    return buf;
}

static char* lpw2text(const lpw_t *p, int conn)
{
    // WARNING: returns single static buffer
    static char buf[80];
    sprintf(buf, "[dcw=0%o ires=%d hrel=%d ae=%d nc=%d trun=%d srel=%d tally=0%o]",
        p->dcw, p->ires, p->hrel, p->ae, p->nc, p->trunout, p->srel, p->tally);
    if (!conn)
        sprintf(buf+strlen(buf), " [lbnd=0%o size=0%o(%d) idcw=0%o]",
            p->lbnd, p->size, p->size, p->idcw);
    return buf;
}

static int parse_lpw(lpw_t *p, int addr, int is_conn)
{
    // NOTE: We always look at 2nd word even if connect channel shouldn't
    p->dcw = M[addr] >> 18;
    p->ires = getbits36(M[addr], 18, 1);
    p->hrel = getbits36(M[addr], 19, 1);
    p->ae = getbits36(M[addr], 20, 1);
    p->nc = getbits36(M[addr], 21, 1);
    p->trunout = getbits36(M[addr], 22, 1);
    p->srel = getbits36(M[addr], 23, 1);
    p->tally = getbits36(M[addr], 24, 12);

    if (!is_conn) {
        // Ignore 2nd word on connect channel
        // following not valid for paged mode; see B15; but maybe IOM-B non existant
        // BUG: look at what bootload does & figure out if they expect 6000-B
        p->lbnd = getbits36(M[addr+1], 0, 9);
        p->size = getbits36(M[addr+1], 9, 9);
        p->idcw = getbits36(M[addr+1], 18, 18);
    } else {
        p->lbnd = -1;
        p->size = -1;
        p->idcw = -1;
    }
}

int lpw_write(int chan, int chanloc, const lpw_t* p)
{
    debug_msg("IOM::lpw_write", "Chan 0%o: Addr 0%o had %012o %012o\n", chan, chanloc, M[chanloc], M[chanloc+1]);
    lpw_t temp;
    parse_lpw(&temp, chanloc, chan == IOM_CONNECT_CHAN);
    //debug_msg("IOM::lpw_write", "Chan 0%o: Addr 0%o had: %s\n", chan, chanloc, lpw2text(&temp, chan == IOM_CONNECT_CHAN));
    //debug_msg("IOM::lpw_write", "Chan 0%o: Addr 0%o new: %s\n", chan, chanloc, lpw2text(p, chan == IOM_CONNECT_CHAN));
    t_uint64 word = 0;
    //word = setbits36(0, 0, 18, p->dcw & MASK18);
    word = setbits36(0, 0, 18, p->dcw);
    word = setbits36(word, 18, 1, p->ires);
    word = setbits36(word, 19, 1, p->hrel);
    word = setbits36(word, 20, 1, p->ae);
    word = setbits36(word, 21, 1, p->nc);
    word = setbits36(word, 22, 1, p->trunout);
    word = setbits36(word, 23, 1, p->srel);
    //word = setbits36(word, 24, 12, p->tally & MASKBITS(12));
    word = setbits36(word, 24, 12, p->tally);
    M[chanloc] = word;

    int is_conn = chan == 2;    // BUG: HACK
    if (!is_conn) {
        t_uint64 word2 = setbits36(0, 0, 9, p->lbnd);
        word2 = setbits36(word2, 9, 9, p->size);
        word2 = setbits36(word2, 18, 18, p->idcw);
        M[chanloc+1] = word2;
    }
    debug_msg("IOM::lpw_write", "Chan 0%o: Addr 0%o now %012o %012o\n", chan, chanloc, M[chanloc], M[chanloc+1]);
    return 0;
}


static int send_chan_flags()
{
    warn_msg("IOM", "send_chan_flags() unimplemented\n");
}


static int status_service(int chan)
{
    // See page 33 and AN87 for format of y-pair of status info

    // BUG: much of the following is not tracked
    
    t_uint64 word1, word2;
    word1 = 0;
    word1 = setbits36(word1, 0, 1, 1);
    word1 = setbits36(word1, 1, 1, chan_status.power_off);
    word1 = setbits36(word1, 2, 4, chan_status.major);
    word1 = setbits36(word1, 6, 6, chan_status.substatus);
    word1 = setbits36(word1, 12, 1, 1); // BUG: even/odd
    word1 = setbits36(word1, 13, 1, 1); // BUG: marker int
    // word1 = setbits36(word1, 14, 2, 0);
    word1 = setbits36(word1, 16, 1, 0); // BUG: initiate flag
    // word1 = setbits36(word1, 17, 1, 0);
    word2 = 0;
#if 0
    word1 = setbits36(word1, 18, 3, chan_status.chan_stat);
    word1 = setbits36(word1, 21, 3, chan_status.iom_stat);
    word1 = setbits36(word1, 24, 6, chan_status.addr_ext);
    word1 = setbits36(word1, 30, 6, chan_status.dcw_residue);

    word2 = setbits36(0, 0, 18, chan_status.addr);
    word2 = setbits36(0, 18, 3, chan_status.char_pos);
    word2 = setbits36(0, 21, 1, chan_status.is_read);
    word2 = setbits36(0, 23, 2, chan_status.type);
    word2 = setbits36(0, 25, 12, chan_status.dcw_residue);
#endif

    // BUG: need to write to mailbox queue

    int chanloc = IOM_A_MBX + chan * 4;
    int scw = chanloc + 2;
    if (scw % 2 == 1) { // 3.2.4
        warn_msg("IOM::status", "SCW address 0%o is not even\n", scw);
        -- scw;         // force y-pair behavior
    }
    int addr = getbits36(M[scw], 0, 18);    // absolute
    debug_msg("IOM::status", "Writing status for chan %d to 0%o\n", chan, addr);
    debug_msg("IOM::status", "Status: 0%012Lo 0%012Lo\n", word1, word2);
    debug_msg("IOM::status", "Status: (0)t=Y, (1)pow=%d, (2..5)major=0%02o, (6..11)substatus=0%02o, (12)e/o=Z, (13)marker=Y, (14..15)Z, 16(Z?), 17(Z)\n",
        chan_status.power_off, chan_status.major, chan_status.substatus);
    int lq = getbits36(M[scw], 18, 2);
    int tally = getbits36(M[scw], 24, 12);
#if 1
    if (lq == 3) {
        warn_msg("IOM::status", "SCW address 0%o has illegal LQ\n", scw);
        lq = 0;
    }
#endif
    M[addr] = word1;
    M[addr+1] = word2;
    switch(lq) {
        case 0:
            // list
            if (tally != 0) {
                addr += 2;
                -- tally;
            }
            break;
        case 1:
            // 4 entry (8 word) queue
            if (tally % 8 == 1 || tally % 8 == -1)
                addr -= 8;
            else
                addr += 2;
            -- tally;
            break;
        case 2:
            // 16 entry (32 word) queue
            if (tally % 32 == 1 || tally % 32 == -1)
                addr -= 32;
            else
                addr += 2;
            -- tally;
            break;
    }
    if (tally < 0 && tally == - (1 << 11) - 1) {    // 12bits => -2048 .. 2047
        warn_msg("IOM::status", "Tally SCW address 0%o wraps to zero\n", tally);
        tally = 0;
    }
    // BUG: update SCW in core
}

static void iom_fault(int chan, int src_line, int is_sys, int signal)
{
    // Store the indicated fault into a system fault word (3.2.6) in
    // the system fault channel -- use a list service to get a DCW to do so

    // sys fault masks channel

    // signal gets put in bits 30..35, but we need fault code for 26..29
    complain_msg("IOM", "Fault for channel %d at line %d: is_sys=%d, signal=%d\n", chan, src_line, is_sys, signal);
    cancel_run(STOP_WARN);
}
