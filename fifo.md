循环FIFO Mailbox设计（针对不满512K的小对象的传输效率高）

# 发送方

一次上层msg到来时：

```
剩余消息 = msg
while (剩余消息 ！= 空){
	head，tail = 接收方buffer的CSR寄存器中的指针
	valid_space = tail2head(head. tail) //返回可写字节数 = 寄存器数 * 8
	if  valid_space != 0 {
		if valid_space <= len(剩余消息) { //剩余消息大于等于当前可用空间
			msg_to_write = 剩余消息[:valid_space]
			剩余消息 = 剩余消息[valid_space:] 
			write(msg_to_write)  //从tail处往寄存器组内写
			/*往控制寄存器更新tail的值等价于发一次中断*/
			tail = head - 1      //模意义下的运算
		} else { //剩余消息小于当前可用空间
			msg_to_write = 剩余消息
			allign (msg_to_write) //将写入的寄存器中的最后一个的结尾补上0（可能有之前的脏数据），按8字节对齐
			剩余消息 = 空
			write(msg_to_write,tail)   //从tail处往内写,写msg_to_write/8个寄存器
			/*等价于发一次中断*/
			tail += BYTES(msg_to_write) / 8  //tail在模意义下增加已使用的格子
		}
	}
}
```



# 接收方

当一次中断到来时：

```
head，tail = 发送方buffer的CSR寄存器中的指针
msg = read(head，tail)  //从head指向的寄存器开始读取 head到tail 个寄存器内的内容
```

传出这个片段化的msg

上层根据msg的具体内容进行组装（本逻辑待完善）
