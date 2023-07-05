from math import ceil
from time import sleep
from queue import Queue
import threading


GREEN = '\033[92m'
RED = '\033[91m'
BLUE = '\033[94m'
END_COLOR = '\033[0m'

MAILBOX_MAX_REG_NUM = 63  # 0-62号寄存器是消息寄存器

MAILBOX_CSR_IDX = 63  # 63号寄存器是控制状态寄存器CSR


# Create two queues to simulate irq
irq1 = Queue(maxsize=1)
irq2 = Queue(maxsize=1)

# Create two buffers to simulate the mailbox buffer
buffer1 = [0] * 64  # Entity1的regs buffer
buffer2 = [0] * 64  # Entity2的regs buffer

"""
CSR寄存器格式：
"{head} {tail}"
"""


def get_head_and_tail_from_CSR(entity_name):
    def parse_control_msg(control_msg):
        head, tail = control_msg.split(" ")
        return int(head), int(tail)
    control_msg = buffer1[MAILBOX_CSR_IDX] if entity_name == "Entity1" else buffer2[MAILBOX_CSR_IDX]
    return parse_control_msg(control_msg)


def unfilled_regs_num(head, tail):
    def filled_regs_num(head, tail):
        return (tail - head + MAILBOX_MAX_REG_NUM) % MAILBOX_MAX_REG_NUM
    return MAILBOX_MAX_REG_NUM - filled_regs_num(head, tail) - 1


class Entity:
    def __init__(self, name, send_irq, receive_irq):
        self.name = name
        self.send_irq = send_irq
        self.receive_irq = receive_irq
        self.sender_thread = threading.Thread(target=self.from_upper_module)
        self.receiver_thread = threading.Thread(target=self.receive)
        if self.name == "Entity1":
            buffer1[MAILBOX_CSR_IDX] = "0 0"
        elif self.name == "Entity2":
            buffer2[MAILBOX_CSR_IDX] = "0 0"

    # 向另一个mailbox的csr写，同时触发中断
    def write_control_msg_to_other_mailbox(self, control_msg):
        if self.name == "Entity1":
            buffer2[MAILBOX_CSR_IDX] = control_msg
        elif self.name == "Entity2":
            buffer1[MAILBOX_CSR_IDX] = control_msg
        # 模拟写入寄存器后后立刻触发中断
        self.send_irq.put_nowait(f"{self.name} send irq")


    # 上层业务代码的抽象,用于模拟上层业务代码发送消息
    def from_upper_module(self):

        msg_from_2_to_1 = ["hello world, this is a long message and it's really l" + "o"*200 + "ng",
                           "animals",
                           "dog",
                           "cat and Cat and cAt and caT and CAT" + " and cats"*60 + " Wow! So many cats!", 
                           "goodbye, world!"
                           ]

        if self.name == "Entity1":
            pass
        elif self.name == "Entity2":
            for i in range(5):
                self.send(f"{msg_from_2_to_1[i]}")
                sleep(1)  # 两次业务间的间隔设置为0.001s (1ms)

    # 上层业务代码的抽象，用于模拟上层业务代码接收消息
    def to_upper_module(self, msg):
        if self.name == "Entity1":
            print("Entity1 received:" + RED + f"{msg}" + END_COLOR)

        elif self.name == "Entity2":
            print(f"Entity2 received:{msg}")

    # 从对方buffer的tail位置开始，将msg写入对方的buffer，直到buffer末尾，再进行循环
    def put_msg(self, msg, tail):
        # 首先将msg按照8字节一块的大小分割
        msg = [msg[i:i+8] for i in range(0, len(msg), 8)]
        for i in range(len(msg)):
            if self.name == "Entity1":
                buffer2[tail] = msg[i]
            elif self.name == "Entity2":
                buffer1[tail] = msg[i]
            tail = (tail + 1) % MAILBOX_MAX_REG_NUM

    # 从自己buffer的head位置开始，读取msg，直到buffer末尾，再进行循环
    def get_msg(self, head, tail):
        msg = ""
        while head != tail:
            if self.name == "Entity1":
                msg += str(buffer1[head])
            elif self.name == "Entity2":
                msg += str(buffer2[head])
            head = (head + 1) % MAILBOX_MAX_REG_NUM
        return msg

    # 上层一个msg到来时
    def send(self, msg: str):
        msg_to_send = msg
        while msg_to_send != "":
            receiver_name = "Entity2" if self.name == "Entity1" else "Entity1"
            head, tail = get_head_and_tail_from_CSR(receiver_name)
            valid_bytes = unfilled_regs_num(head, tail) * 8
            if valid_bytes != 0:
                print(f"{self.name} send:{msg_to_send}")
                if valid_bytes <= len(msg_to_send):  # 说明可以将buffer填满
                    msg_to_write = msg_to_send[:valid_bytes]
                    msg_to_send = msg_to_send[valid_bytes:]
                else:  # 说明剩余部分不足以填满buffer
                    msg_to_write = msg_to_send
                    # 将msg_to_write按照到8字符对齐
                    msg_to_write += " " * (8 - len(msg_to_write) % 8)
                    msg_to_send = ""
                self.put_msg(msg_to_write, tail)
                tail = (tail + len(msg_to_write) // 8) % MAILBOX_MAX_REG_NUM
                print(f"{receiver_name}" + BLUE + f" head_reg:{head}, tail_reg:{tail}" + END_COLOR)
                self.write_control_msg_to_other_mailbox(
                    f"{head} {tail}")

    # 监听并处理中断
    def receive(self):
        while True:
            # 模拟等待中断到来
            self.receive_irq.get()  # This will block until a irq is received
            head, tail = get_head_and_tail_from_CSR(self.name)
            msg = self.get_msg(head, tail)
            head = tail  # 读取完毕后，将head指针指向tail，表示本次已经读空
            self.to_upper_module(msg)
            if self.name == "Entity1":
                buffer1[MAILBOX_CSR_IDX] = f"{head} {tail}"
            else:
                buffer2[MAILBOX_CSR_IDX] = f"{head} {tail}"

    def start(self):
        self.sender_thread.start()
        self.receiver_thread.start()


if __name__ == "__main__":

    # Create two entities, each with a send queue and a receive queue
    entity1 = Entity("Entity1", send_irq=irq1, receive_irq=irq2)
    entity2 = Entity("Entity2", send_irq=irq2, receive_irq=irq1)

    # Start both entities
    entity1.start()
    entity2.start()
