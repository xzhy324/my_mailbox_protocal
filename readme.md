# mailbox底层通信协议的逻辑设计与基于线程的模拟

1. 适用于大块数据（0.5KB的若干倍）的同步通信协议 `mailbox_sync.py`

   将数据分块，一块填满所有mailbox寄存器，传输后需要接收方确认，下一个数据块才过来

2. 适用于数据流以及小块数据的循环FIFO通信协议 `mailbox_fifo.py`

   每次上层应用msg到来时，根据当前循环队列首尾指针情况放入mailbox

   伪代码实现见`fifo.md`

## 测例说明

函数from_upper_module用来模拟上层应用发消息的行为。
上层应用发送5个消息给对方，这些消息有长有短，包括：

1. 整个mailbox都装不下的消息
2. 只装了一个或者几个寄存器的消息
3. 装了很多寄存器并导致了序号回环的消息

使用to_upper_module验证接收到的效果

## 运行

tested on python >=3.8

```shell
python mailbox_fifo.py
```

