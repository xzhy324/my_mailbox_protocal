// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! ASP Mailbox driver.
#![no_std]
#![allow(clippy::missing_safety_doc)]



use cantrip_os_common::camkes::Camkes;
use cantrip_os_common::sel4_sys;
use core::{cmp, char::MAX};
use sel4_sys::seL4_PageBits;
use log::{error, trace, info};

// TODO(chrisphan): Use ringbuf crate instead.
//use circular_buffer::Buffer;
//use register::{bit, Field, Register};


// Driver-owned circular buffer to receive more than the FIFO size before the
// received data is consumed by rx_update.
//static mut RX_BUFFER: Buffer = Buffer::new();

// Driver-owned circular buffer to buffer more transmitted bytes than can fit
// in the transmit FIFO.
//static mut TX_BUFFER: Buffer = Buffer::new();


const ENABLE_BIT: u64 = (1 as u64) << 63;
const VALID_MASK: u64 = 0x7fff_ffff_ffff_ffff;
const RECEIVE_BASE_INDEX: isize = 64;
const RECEIVE_CSR_INDEX : isize = 127;
const RECEIVE_IR_INDEX: isize = 126;
const SEND_CSR_INDEX : isize = 63;
const SEND_IR_INDEX: isize = 62;
const MAILBOX_MAX_REG_NUM: u64 = 62;

static mut CAMKES: Camkes = Camkes::new("ASPMailboxDriver");

extern "C" {
    static mmio_region: *mut u64;
    fn api_mutex_lock() -> u32;
    fn api_mutex_unlock() -> u32;
    fn rx_semaphore_wait() -> u32;
    fn rx_semaphore_post() -> u32;
    fn rx_irq_acknowledge() -> u32;
}

/// Assert while preserving expr in non-debug mode.
#[inline(never)]
fn cantrip_assert(expr: bool) {
    debug_assert!(expr);
}

unsafe fn read_reg(index :isize ) -> u64{mmio_region.offset(index).read_volatile()}
unsafe fn write_reg(val :u64, index:isize ) {mmio_region.offset(index).write_volatile(val);}

#[no_mangle]
#[allow(unused_variables)]
pub fn logger_log(_level: u8, msg: *const cstr_core::c_char) {
    unsafe {
        for c in cstr_core::CStr::from_ptr(msg).to_bytes() {
            let _ = sel4_sys::seL4_DebugPutChar(*c);
        }
        let _ = sel4_sys::seL4_DebugPutChar(b'\n');
    }
}
/// Performs initial programming of the OpenTitan UART at mmio_region.
#[no_mangle]
pub unsafe extern "C" fn pre_init() {
    CAMKES.init_logger(log::LevelFilter::Trace);
    write_reg(ENABLE_BIT | VALID_MASK,RECEIVE_CSR_INDEX);
    log::info!("ASP Mailbox initialized, ver=003");
    let val = read_reg(RECEIVE_CSR_INDEX);
    log::info!("receive csr value is now 0x{:X}",val);
  
}


//中断来临时走此函数
#[no_mangle]
pub unsafe extern "C" fn rx_irq_handle() {
    api_mutex_lock();

    let valid_bits = read_reg(RECEIVE_CSR_INDEX) & VALID_MASK;

    //log::info!("[test]interrupt handler: valid_bits value is 0x{:X}",valid_bits);
    write_reg(valid_bits | ENABLE_BIT,RECEIVE_CSR_INDEX); //开中断
    // let tmp = read_reg(receive_csr_index);
    // //log::info!("[test]interrupt handler: Receive Complete");
    // log::info!("[test]interrupt handler: receive csr value is 0x{:X}",tmp);
    let receive_info_reg = read_reg(RECEIVE_IR_INDEX);
    log::info!("[test]interrupt handler: receive ir value is 0x{:X}",receive_info_reg);
    let send_info_reg = read_reg(SEND_IR_INDEX);
    log::info!("[test]interrupt handler: send ir value is 0x{:X}",send_info_reg);
    let rx_tail_from_receiver = (receive_info_reg & 0xff00) >> 8;
    let rx_head_from_sender = send_info_reg & 0x00ff;

    let mut msgs: [u64; 62] = [0;62];
    let mut msg_ptr:usize = 0;

    let mut head = rx_head_from_sender;

    while head != rx_tail_from_receiver {
        // let tmp_addr = RECEIVE_BASE_INDEX + (head as isize);
        // log::info!("[test]interrupt handler: read from {}",tmp_addr);
        msgs[msg_ptr] = read_reg(RECEIVE_BASE_INDEX + (head as isize));/* error */
        head += 1;
        if head >= MAILBOX_MAX_REG_NUM {
            head -= MAILBOX_MAX_REG_NUM;
        }
        msg_ptr += 1;
    }

    log::info!("[test]interrupt handler: len={} msgs: {:?}",msg_ptr, msgs);

    let mut send_info_reg = read_reg(SEND_IR_INDEX);
    send_info_reg &= 0xffff_ffff_ffff_ff00;
    send_info_reg |= rx_tail_from_receiver;
    write_reg(send_info_reg, SEND_IR_INDEX);


    api_mutex_unlock();
    cantrip_assert(rx_irq_acknowledge() == 0);
}




