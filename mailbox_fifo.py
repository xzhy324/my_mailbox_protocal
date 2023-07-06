"""
CSR寄存器格式:
"{irq} {rx_head} {rx_tail} {tx_head} {tx_tail}"
irq = 1 表示有中断,立刻触发中断处理逻辑receive
rx_*: 自己的接收buffer的头尾指针，自己不能够修改
tx_*: 对方的接收buffer的头尾指针，自己可以修改

采用空1个位置的循环FIFO,约定如下
1. 首尾指针相等,表示buffer为空
2. 首尾指针相差1,表示buffer满  => 最多只有63 - 1 = 62个寄存器可用
3. 首尾指针均初始化为0 (或其他任意相同值)
4. 在head != tail的情况下,head指向的位置是满的,否则为空
5. 任何情况下,tail指向的位置都是空的

中断触发时机：
当生产者更新了tail指针时,触发中断, 通知对方开始消费
当消费者更新了head指针时,不触发中断, 此时生产者通过轮询的方式检查head指针是否更新
"""

import threading
from queue import Queue
from time import sleep

GREEN = '\033[92m'
RED = '\033[91m'
BLUE = '\033[34m'
END_COLOR = '\033[0m'


MAILBOX_MAX_REG_NUM = 63  # 0-62号寄存器是消息寄存器
MAILBOX_CSR_IDX = 63  # 63号寄存器是控制状态寄存器CSR


# Create two queues to simulate irq
irq1 = Queue()
irq2 = Queue()

# Create two buffers to simulate the mailbox buffer
buffer1 = [0] * 64  # Entity1的regs buffer
buffer2 = [0] * 64  # Entity2的regs buffer


# 根据核的名称返回其接收buffer中的头尾指针
def get_head_and_tail_from_CSR(entity_name):
    control_msg = buffer1[MAILBOX_CSR_IDX] if entity_name == "Entity1" else buffer2[MAILBOX_CSR_IDX]
    irq, head, tail = control_msg.split(" ")
    return int(head), int(tail)

# 通过head和tail的值计算出可使用的寄存器数量
def get_unfilled_regs_num(head, tail):
    def filled_regs_num(head, tail):
        return (tail - head + MAILBOX_MAX_REG_NUM) % MAILBOX_MAX_REG_NUM
    return MAILBOX_MAX_REG_NUM - filled_regs_num(head, tail) - 1


class Entity:
    def __init__(self, name, send_irq, receive_irq, tx_buffer, rx_buffer):
        self.name = name
        self.send_irq = send_irq            # 向对方发送的中断信号（在向对方rx_regs写的时候触发）
        self.receive_irq = receive_irq      # receive监听的中断信号
        self.tx_buffer = tx_buffer          # 对方用于接收的regs buffer
        self.rx_buffer = rx_buffer          # 自己用于接收的regs buffer
        self.sender_thread = threading.Thread(target=self.from_upper_module)    # 写消息线程
        self.receiver_thread = threading.Thread(target=self.receive)            # 中断监听线程
        self.rx_buffer[MAILBOX_CSR_IDX] = "0 0 0"  # 初始状态: 没有中断, head=0, tail=0 
        self.recv_msgs = []

    # 向另一个mailbox的csr写，根据最高位决定是否触发中断
    def write_control_msg_to_other_mailbox(self, control_msg):
        self.tx_buffer[MAILBOX_CSR_IDX] = control_msg
        # 若最高位为1，触发对方mailbox的中断
        if control_msg.split()[0] == "1":
            self.send_irq.put_nowait(f"{self.name} send irq")

    # 上层业务代码的抽象,用于模拟上层业务代码发送消息
    def from_upper_module(self):
        if self.name == "Entity1":
            print("Entity1 wants to send" + GREEN + f"{msg_from_1_to_2}" + END_COLOR)
            for i in range(len(msg_from_1_to_2)):
                print(f"{self.name} send:" + BLUE + f"{msg_from_1_to_2[i]}" + END_COLOR)
                self.send(f"{msg_from_1_to_2[i]}")
                sleep(1 / REQUEST_PER_SECOND)
        elif self.name == "Entity2":
            print("Entity2 wants to send" + GREEN + f"{msg_from_2_to_1}" + END_COLOR)
            for i in range(len(msg_from_2_to_1)):
                print(f"{self.name} send:" + BLUE + f"{msg_from_2_to_1[i]}" + END_COLOR)
                self.send(f"{msg_from_2_to_1[i]}")
                sleep(1 / REQUEST_PER_SECOND)

    # 上层业务代码的抽象，用于模拟上层业务代码接收消息
    def to_upper_module(self, msg):
        print(f"{self.name} received:" + BLUE + f"{msg}" + END_COLOR)
        self.recv_msgs.append(msg)
        if len(self.recv_msgs) == 8:
            print(f"{self.name} finally received:" + GREEN + f"{self.recv_msgs}" + END_COLOR)
        
    # 从对方buffer的tail位置开始，将msg在模M意义下顺序写入对方的buffer
    def put_msg(self, msg, tail):
        # 首先将msg按照8字节一块的大小分割
        msg = [msg[i:i+8] for i in range(0, len(msg), 8)]
        for i in range(len(msg)):
            self.tx_buffer[tail] = msg[i]
            tail = (tail + 1) % MAILBOX_MAX_REG_NUM

    # 从自己buffer的head位置开始，读取msg，直到tail位置（模M意义下）
    def get_msg(self, head, tail):
        msg = ""
        while head != tail:
            msg += str(self.rx_buffer[head])
            head = (head + 1) % MAILBOX_MAX_REG_NUM
        return msg

    # 上层一个msg到来时
    def send(self, msg: str):
        msg_to_send = msg
        while msg_to_send != "":
            receiver_name = "Entity2" if self.name == "Entity1" else "Entity1"
            head, tail = get_head_and_tail_from_CSR(receiver_name)
            valid_bytes = get_unfilled_regs_num(head, tail) * 8
            if valid_bytes != 0:  # 只有在对方buffer有空余的时候才能发送，否则一直轮询直到有空余
                # print(f"valid {head}:{tail} {self.name} send:{msg_to_send}")
                if valid_bytes <= len(msg_to_send):  # 说明可以将buffer填满
                    msg_to_write = msg_to_send[:valid_bytes] 
                    msg_to_send = msg_to_send[valid_bytes:]  
                else:  # 说明剩余部分不足以填满buffer
                    msg_to_write = msg_to_send
                    msg_to_send = ""
                    msg_to_write += " " * (8 - len(msg_to_write) % 8)  # 将msg_to_write按照到8字符对齐
                    
                self.put_msg(msg_to_write, tail)
                #print(f"{self.name}'s tx_buffer" + GREEN + f"{self.tx_buffer}" + END_COLOR)
                tail = (tail + len(msg_to_write) // 8) % MAILBOX_MAX_REG_NUM
                # print(f"{self.name} try to write {receiver_name}'s CSR" + BLUE +
                #       f" head_reg:{head}, tail_reg:{tail}" + END_COLOR)
                head, tmp = get_head_and_tail_from_CSR(receiver_name)# 写回前临时更新一下 head（在过程中head可能已经被修改）
                self.write_control_msg_to_other_mailbox(
                    f"1 {head} {tail}")  # 写回对方的csr，触发中断

    # 监听并处理中断
    def receive(self):
        while True:
            # 阻塞等待中断到来
            self.receive_irq.get()  # This will block until a irq is received
            self.rx_buffer[MAILBOX_CSR_IDX] = "0" + self.rx_buffer[MAILBOX_CSR_IDX][1:]  # 将自己中断的最高位置零， 其余信息不变
            
            head, tail = get_head_and_tail_from_CSR(self.name)
            msg = self.get_msg(head, tail)
            head = tail  # 读取完毕后，将head指针指向tail，表示本次已经将自己有消息的寄存器组读空
            self.to_upper_module(msg)  # 给上层应用传递消息
            # print(f"{self.name} try to write {self.name}'s CSR" + BLUE + f" head_reg:{head}, tail_reg:{tail}" + END_COLOR)
            tmp, tail = get_head_and_tail_from_CSR(self.name)  # 写回前临时更新一下 tail（在过程中tail可能已经被修改），实际不需要，因为只修改寄存器的特定位而不是全部写回
            self.rx_buffer[MAILBOX_CSR_IDX] = f"0 {head} {tail}"  # 更新自己的csr, 但是不触发中断

    def start(self):
        self.sender_thread.start()
        self.receiver_thread.start()


if __name__ == "__main__":
    
    REQUEST_PER_SECOND = 100000  # 每秒发送的消息数
    msg_from_2_to_1 = ["hello world, this is a long message and it's really l" + "o"*200 + "ng",
                        "animals",
                        "dog",
                        "cat and Cat and cAt and caT and CAT" + " and cats"*60 + " Wow! So many cats!",
                        "goodbye, world!",
                        "hello world, this is a long message and it's really l" + "oO"*300 + "ng",]
    # msg_from_1_to_2 = ["HELLO WORLD, THIS IS A LONG MESSAGE AND IT'S REALLY L" + "O"*200 + "NG",
    #                    "ANIMALS",
    #                    "DOG",
    #                    "CAT AND CAT AND CAT AND CAT AND CAT" + " AND CATS"*60 + " WOW! SO MANY CATS!",
    #                    "GOODBYE, WORLD!"]
    msg_from_1_to_2 = []

    # Create two entities, each with a send queue and a receive queue
    entity1 = Entity("Entity1", send_irq=irq1, receive_irq=irq2,
                     tx_buffer=buffer2, rx_buffer=buffer1)
    entity2 = Entity("Entity2", send_irq=irq2, receive_irq=irq1,
                     tx_buffer=buffer1, rx_buffer=buffer2)

    # Start both entities
    entity1.start()
    entity2.start()
