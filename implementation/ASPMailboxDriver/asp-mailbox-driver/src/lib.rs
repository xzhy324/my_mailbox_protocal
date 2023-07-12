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
use log::info;

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

const RECEIVE_BASE_INDEX: isize = 64;//我们自己的接受区，只读
const RECEIVE_CSR_INDEX: isize = 127;
const RECEIVE_IR_INDEX: isize = 126;

const SEND_BASE_INDEX: isize = 0; //我们自己的发送区，可读写，即对方的接收区
const SEND_CSR_INDEX: isize = 63;
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

unsafe fn read_reg(index: isize) -> u64 { mmio_region.offset(index).read_volatile() }
unsafe fn write_reg(val: u64, index: isize) { mmio_region.offset(index).write_volatile(val); }

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
    write_reg(ENABLE_BIT | VALID_MASK, RECEIVE_CSR_INDEX);
    log::info!("ASP Mailbox initialized, ver=003");
    let val = read_reg(RECEIVE_CSR_INDEX);
    log::info!("receive csr value is now 0x{:X}", val);
}

//中断来临时走此函数
#[no_mangle]
pub unsafe extern "C" fn rx_irq_handle() {
    api_mutex_lock();

    let valid_bits = read_reg(RECEIVE_CSR_INDEX) & VALID_MASK;

    //log::info!("[test]interrupt handler: valid_bits value is 0x{:X}",valid_bits);
    write_reg(valid_bits | ENABLE_BIT, RECEIVE_CSR_INDEX); //开中断
    // let tmp = read_reg(receive_csr_index);
    // //log::info!("[test]interrupt handler: Receive Complete");
    // log::info!("[test]interrupt handler: receive csr value is 0x{:X}",tmp);
    let receive_info_reg = read_reg(RECEIVE_IR_INDEX);
    log::info!(
        "[test]interrupt handler: receive ir value is 0x{:X}",
        receive_info_reg
    );
    let send_info_reg = read_reg(SEND_IR_INDEX);
    log::info!("[test]interrupt handler: send ir value is 0x{:X}", send_info_reg);
    let rx_tail_from_receiver = (receive_info_reg & 0xff00) >> 8;
    let rx_head_from_sender = send_info_reg & 0x00ff;

    let mut msgs: [u64; 62] = [0; 62];
    let mut msg_ptr: usize = 0;

    let mut head = rx_head_from_sender;

    while head != rx_tail_from_receiver {
        // let tmp_addr = RECEIVE_BASE_INDEX + (head as isize);
        // log::info!("[test]interrupt handler: read from {}",tmp_addr);
        msgs[msg_ptr] = read_reg(RECEIVE_BASE_INDEX + (head as isize)); /* error */
        head += 1;
        if head >= MAILBOX_MAX_REG_NUM {
            head -= MAILBOX_MAX_REG_NUM;
        }
        msg_ptr += 1;
    }

    log::info!("[test]interrupt handler: len={} msgs: {:?}", msg_ptr, msgs);

    let mut send_info_reg = read_reg(SEND_IR_INDEX);
    send_info_reg &= 0xffff_ffff_ffff_ff00;
    send_info_reg |= rx_tail_from_receiver; //将head设置为原来的tail,即读出了所有内容
    write_reg(send_info_reg, SEND_IR_INDEX);

    api_mutex_unlock();
    cantrip_assert(rx_irq_acknowledge() == 0);
}

// 阻塞发送消息
pub unsafe fn block_send(msg: &[u64]) {
    let size: usize = msg.len();
    let mut msg_ptr: usize = 0;

    /* 待实现逻辑：若接收方关闭了中断使能，则不发送消息 */
    //let mailbox_csr = read_reg(RECEIVE_CSR_INDEX);

    while msg_ptr != size {
        let receiver_info_reg = read_reg(SEND_IR_INDEX);
        let sender_info_reg = read_reg(RECEIVE_IR_INDEX);
        let rx_tail_from_receiver = (receiver_info_reg & 0xff00) >> 8;
        let rx_head_from_sender = sender_info_reg & 0x00ff;
        let used_regs_num = u64_mod(rx_tail_from_receiver + MAILBOX_MAX_REG_NUM  - rx_head_from_sender, MAILBOX_MAX_REG_NUM); //先相加保证usize一定为正数
        let valid_regs_num = MAILBOX_MAX_REG_NUM - 1 - used_regs_num;

        if valid_regs_num > 0 {
            log::info!("valid regs are:{}", valid_regs_num);
            let regs_to_write = u64_min((size - msg_ptr) as u64, valid_regs_num);
            for i in 0..regs_to_write {
                let val = msg[msg_ptr + i as usize];
                let index = u64_mod(SEND_BASE_INDEX as u64 + rx_tail_from_receiver + i, MAILBOX_MAX_REG_NUM);
                write_reg(val, index as isize);
            }
            msg_ptr += regs_to_write as usize;
            let new_rx_tail = u64_mod(rx_tail_from_receiver + regs_to_write, MAILBOX_MAX_REG_NUM);
            let mut receiver_info_reg = read_reg(SEND_IR_INDEX);
            receiver_info_reg &= 0xffff_ffff_ffff_00ff;
            receiver_info_reg |= new_rx_tail << 8;
            log::info!("[test]fn block_send: writing {} to linux's mailbox csr",receiver_info_reg);
            write_reg(receiver_info_reg, SEND_IR_INDEX);
            write_reg(0xffff_ffff_ffff_ffff, SEND_CSR_INDEX);
        }
    }
}

#[inline]
fn u64_min(a:u64,b:u64) -> u64{if a < b {a} else {b} }
#[inline]
fn u64_mod(val:u64, m:u64) -> u64 {if val < m {val} else {val-m} }
