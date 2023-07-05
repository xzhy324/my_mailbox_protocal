from math import ceil
from time import sleep
from queue import Queue
import threading


GREEN = '\033[92m'
RED = '\033[91m'
END_COLOR = '\033[0m'

MAILBOX_MAX_SIZE = 504


# Create two queues to simulate irq
irq1 = Queue(maxsize=1)
irq2 = Queue(maxsize=1)

# Create two buffers to simulate the mailbox buffer
buffer1 = [0] * 64  # Entity1的buffer
buffer2 = [0] * 64  # Entity2的buffer

'''
              一次msg传递的顺序
              
from_upper_module          to_upper_module        上层业务,(或许可以将这块变成一个FIFO队列，上层任务向这个FIFO队列提交自己要发送的消息【kv形式】)
         |                     ^
         v                     |
slice_msg_and_send      resemble_msg_and_moveup   负责将>512B的消息进行分块与组装, 多次调用send与receive_signal
         |                     ^
         v                     |
        send   ----------> receive_irq            同步传输一块消息(最多512B)
        
CSR寄存器格式：
1. "{start} {end} {current_block_idx} {total_block_num}"
2. "ack"
'''


def parse_control_msg(control_msg):
    start, end, current_block_idx, total_block_num = control_msg.split(" ")
    return int(start), int(end), int(current_block_idx), int(total_block_num)


class Entity:
    def __init__(self, name, send_irq, receive_irq):
        self.name = name
        self.send_irq = send_irq
        self.receive_irq = receive_irq
        self.sender_thread = threading.Thread(target=self.from_upper_module)
        self.receiver_thread = threading.Thread(target=self.receive)
        self.can_send = threading.Event()
        self.can_send.set()  # 初始状态可以发送
        self.resembled_msg_queue = []  # 将待组装队列置为空

    # 向另一个mailbox的csr写，同时触发中断
    def write_control_msg_to_other_mb(self, control_msg):
        if self.name == "Entity1":
            buffer2[0] = control_msg
        elif self.name == "Entity2":
            buffer1[0] = control_msg
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
                self.slice_msg_and_send(f"{msg_from_2_to_1[i]}")
                sleep(0.001)  # 两次业务间的间隔设置为0.001s (1ms)

    # 上层业务代码的抽象，用于模拟上层业务代码接收消息
    def to_upper_module(self, msg):
        if self.name == "Entity1":
            print("Entity1 received:" + RED + f"{msg}" + END_COLOR)

        elif self.name == "Entity2":
            print(f"Entity2 received:{msg}")

    # 将msg分块后调用send发送(可调用多次send)
    def slice_msg_and_send(self, msg):
        # 将msg每MAILBOX_MAX_SIZE个字符分成一块,分别使用send发送
        total_block_num = ceil(len(msg) / MAILBOX_MAX_SIZE)
        msg_blocks = [msg[i:i+MAILBOX_MAX_SIZE]
                      for i in range(0, len(msg), MAILBOX_MAX_SIZE)]
        for current_block_idx, msg_block in enumerate(msg_blocks):
            self.send(msg_block, current_block_idx + 1, total_block_num)

    # 从mailbox拿到一块msg之后，尝试拼接成完整的msg，如果拼接成功，调用to_upper_module
    # 这里自己需要维护一个队列，队列有三个状态：空队列，拼接中，拼接完成
    def resemble_msg_and_moveup(self, msg, current_block_idx, total_block_num):
        # 从msg中获取当前msg处于的块的编号，以及总体有多少块
        if current_block_idx < total_block_num:
            self.resembled_msg_queue.append(msg)
        elif current_block_idx == total_block_num:
            self.resembled_msg_queue.append(msg)
            self.to_upper_module("".join(self.resembled_msg_queue))
            self.resembled_msg_queue = []

    # 向处理区写,可能因上一个任务没有完成而阻塞
    def send(self, msg: str, current_block_idx: int = 1, total_block_num: int = 1):
        self.can_send.wait()  # 这里也可以替换为一个强制等待，让发送方等待接收方处理完毕上一次消息,需要知道接收方的处理时延大概是多少
        self.can_send.clear()  # 发送方开始发送消息，将can_send置为False
        # sleep(0.002) # 这里的时间应设置为处理时延

        num_block = ceil(len(msg) / 8)
        # 将msg每八个字符分成一块,构造一个新的变量msg_blocks（最后一块填不满就补‘0’）
        msg_blocks = [msg[i:i+8] for i in range(0, len(msg), 8)]

        start = 1  # 0号寄存器留给控制信息
        end = start + num_block

        for i in range(start, end):
            if self.name == "Entity1":
                buffer2[i] = msg_blocks[i - start]
            elif self.name == "Entity2":
                buffer1[i] = msg_blocks[i - start]

        self.write_control_msg_to_other_mb(
            f"{start} {end} {current_block_idx} {total_block_num}")

    # 监听并处理中断
    def receive(self):
        while True:
            # 模拟等待中断到来
            self.receive_irq.get()  # This will block until a irq is received
            # 中断到来后，首先读自己的状态寄存器
            if self.name == "Entity1":
                control_msg = buffer1[0]
            elif self.name == "Entity2":
                control_msg = buffer2[0]

            if control_msg == "ack":  # 如果设置了发送方强制等待时延，这里的判断就可以删去
                self.can_send.set()  # 发送方可以继续发送消息
                continue
            if self.name == "Entity1":
                print("Got signal! Entity1's buffer:" +
                      GREEN + f"{buffer1}" + END_COLOR)
            # 解析control_msg，得到当前消息的起止位置，以及当前块的编号和总块数
            start, end, current_block_idx, total_block_num = parse_control_msg(
                control_msg)
            # 取出msg
            msg = buffer1[start:end]
            # 将msg拼接成完整的消息
            msg = "".join(msg)
            # 传给上层应用
            self.resemble_msg_and_moveup(
                msg, current_block_idx, total_block_num)
            # 回复一个ack ，如果设置了发送方强制等待时延，这里也可以不回复
            self.write_control_msg_to_other_mb("ack")
            #print("After dealing, Entity1's buffer:", buffer1)

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
