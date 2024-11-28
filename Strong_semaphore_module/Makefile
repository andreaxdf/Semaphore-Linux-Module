obj-m += the_strong_semaphore_module.o
the_strong_semaphore_module-objs += strong_semaphore_module.o lib/scth.o

SYS_CALL_TABLE = $(shell cat /sys/module/the_usctm/parameters/sys_call_table_address)


all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules 

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

mount:
	sudo insmod the_strong_semaphore_module.ko the_syscall_table=$(SYS_CALL_TABLE)

unmount:
	sudo rmmod the_strong_semaphore_module.ko
	