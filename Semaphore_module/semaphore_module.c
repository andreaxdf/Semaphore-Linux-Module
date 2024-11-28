
#define EXPORT_SYMTAB
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/kprobes.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/page.h>
#include <asm/cacheflush.h>
#include <asm/apic.h>
#include <asm/io.h>
#include <linux/syscalls.h>
#include "lib/include/scth.h"

#define MODNAME "SYNCRONIZATION"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrea De Filippis");
MODULE_DESCRIPTION("sync service");

#define AUDIT if(1)

#define NO (0)
#define YES (NO+1)

unsigned long the_syscall_table = 0x0;
module_param(the_syscall_table, ulong, 0660);

unsigned long new_sys_call_array[] = {0x0,0x0,0x0};//It will set to the three syscall at startup

unsigned long the_ni_syscall;

static int enable_sleep = 1; // this can be configured at run time via the sys file system - 1 meas any sleeping thread is freezed
module_param(enable_sleep,int,0660);

typedef struct _semaphore {
        int sem_ds; // Semaphore "descriptor"
        volatile int tokens; // Semaphore tokens
        struct wait_queue_head *wait_queue;
        spinlock_t *sem_lock;
        struct _semaphore *next;
} semaphore;

// Could be used a tree for a better implementation 
semaphore semaphore_list_head = {-1,-1,NULL,NULL,NULL};
semaphore *semaphore_list_tail = &semaphore_list_head; // This pointer will point at the last semaphore in the list so that the creation of a new semaphore 
int last_descriptor = -1;
spinlock_t semaphore_list_lock;

semaphore *getSemaphore(int sem_ds) {
        semaphore *curr = &semaphore_list_head;

        while(curr != NULL) {
                if(curr->sem_ds == sem_ds) {
                        return curr;
                }
                curr = curr->next;
        }

        return NULL;
}

// Returns 0 if the attempt is successful, 1 otherwise
int try_to_lock(semaphore *sem, unsigned int tokens) {
        int ret = 1;

        AUDIT
        printk("%s - THREADS %d: trying to lock semaphore %d\n", MODNAME, current->pid, sem->sem_ds);
        
        preempt_disable();
        spin_lock(sem->sem_lock);

        if(sem->tokens >= tokens) {
                sem->tokens -= tokens;
                ret = 0;

                AUDIT
                printk("%s - THREADS %d: semaphore %d successfully locked. Now it has %d tokens.\n", MODNAME, current->pid, sem->sem_ds, sem->tokens);
        }
        else {
                AUDIT
                printk("%s - THREADS %d: failed locking semaphore %d. It has only %d tokens.\n", MODNAME, current->pid, sem->sem_ds, sem->tokens);
        }

        spin_unlock(sem->sem_lock);
        preempt_enable();

        return ret;
}

int goto_sleep(semaphore *sem, int tokens){

        if(!enable_sleep){
                printk("%s: sys_goto_sleep - sleeping not currently enabled\n",MODNAME);
                return -1;
        }

        AUDIT
        printk("%s: thread %d actually going to sleep\n",MODNAME,current->pid);
        
        do {
                wait_event_interruptible(*(sem->wait_queue), sem->tokens >= tokens);

                AUDIT
                printk("%s: thread %d woke up. Trying to lock semaphore %d \n",MODNAME,current->pid,sem->sem_ds);
        } while(try_to_lock(sem, tokens));

        AUDIT
        printk("%s: thread %d \n",MODNAME, current->pid);

        return 0;
}

#define HACKED_ENTRIES 3
int restore[HACKED_ENTRIES] = {[0 ... (HACKED_ENTRIES-1)] -1};

// tokens: number of token to inizialize the semaphore
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
__SYSCALL_DEFINEx(1, _create_semaphore, int, tokens){
#else
asmlinkage long sys_create_semaphore(int tokens){
#endif

        semaphore *new_sem; 
        spinlock_t *sem_lock;
        struct wait_queue_head *queue;

        AUDIT
        printk("%s: creating a new semaphore",MODNAME);

        new_sem = (semaphore *)kmalloc(sizeof(semaphore), GFP_KERNEL);
        queue = (struct wait_queue_head *)kmalloc(sizeof(struct wait_queue_head), GFP_KERNEL);
        sem_lock = (spinlock_t *)kmalloc(sizeof(spinlock_t), GFP_KERNEL);

        if(new_sem == NULL || queue == NULL || sem_lock == NULL) {
                AUDIT
                printk("%s: unable to allocate memory with kmalloc",MODNAME);
                return -1;
        }

        init_waitqueue_head(queue);

        new_sem->next = NULL;
        new_sem->tokens = tokens;
        new_sem->wait_queue = queue;
        spin_lock_init(sem_lock);
        new_sem->sem_lock = sem_lock;
        
        spin_lock(&semaphore_list_lock);

        last_descriptor++;
        new_sem->sem_ds = last_descriptor;
        semaphore_list_tail->next = new_sem;
        semaphore_list_tail = new_sem;

        spin_unlock(&semaphore_list_lock);

        AUDIT
        printk("%s: created a new semaphore with ds=%d and tokens=%d\n",MODNAME, new_sem->sem_ds, new_sem->tokens);

        return new_sem->sem_ds;
}

// sem_ds: semaphore "descriptor" returned by create semaphore syscall
// tokens: number of token to take
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
__SYSCALL_DEFINEx(2, _sem_lock, int, sem_ds, int, tokens){
#else
asmlinkage long sys_sem_lock(int tokens){
#endif
        semaphore *sem = getSemaphore(sem_ds);

        if(sem == NULL) return -1;

        goto_sleep(sem, tokens);

        return 0;
}




// tokens: number of token to release
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
__SYSCALL_DEFINEx(2, _sem_unlock, int, sem_ds, int, tokens){
#else
asmlinkage long sys_sem_unlock(int sem_ds, int tokens){
#endif

        semaphore *sem;

        AUDIT
        printk("%s - THREADS %d: trying to release %d tokens on semaphore %d", MODNAME, current->pid, tokens, sem_ds);

        sem = getSemaphore(sem_ds);

        if(sem == NULL) return -1;

        preempt_disable();
        spin_lock(sem->sem_lock);

        sem->tokens += tokens;

        spin_unlock(sem->sem_lock);
        preempt_enable();

        AUDIT
        printk("%s: released %d tokens on semaphore %d. Now it has %d tokens", MODNAME, tokens, sem_ds, sem->tokens);

        wake_up(sem->wait_queue);

        AUDIT
        printk("%s: woke up threads in wait queue on semaphore %d", MODNAME, sem_ds);

        return 0;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,17,0)
long sys_create_semaphore = (unsigned long) __x64_sys_create_semaphore;
long sys_sem_lock = (unsigned long) __x64_sys_sem_lock;
long sys_sem_unlock = (unsigned long) __x64_sys_sem_unlock;
#endif


int init_module(void) {

        int i;
        int ret;

        if (the_syscall_table == 0x0){
                printk("%s: cannot manage sys_call_table address set to 0x0\n",MODNAME);
                return -1;
        }

        AUDIT{
                printk("%s: queuing example received sys_call_table address %px\n",MODNAME,(void*)the_syscall_table);
                printk("%s: initializing - hacked entries %d\n",MODNAME,HACKED_ENTRIES);
        }

        new_sys_call_array[0] = (unsigned long)sys_create_semaphore;
        new_sys_call_array[1] = (unsigned long)sys_sem_lock;
        new_sys_call_array[2] = (unsigned long)sys_sem_unlock;

        ret = get_entries(restore,HACKED_ENTRIES,(unsigned long*)the_syscall_table,&the_ni_syscall);

        if (ret != HACKED_ENTRIES){
                printk("%s: could not hack %d entries (just %d)\n",MODNAME,HACKED_ENTRIES,ret);
                return -1;
        }

        unprotect_memory();

        for(i=0;i<HACKED_ENTRIES;i++){
                ((unsigned long *)the_syscall_table)[restore[i]] = (unsigned long)new_sys_call_array[i];
        }

        protect_memory();

        spin_lock_init(&semaphore_list_lock);

        printk("%s: all new system-calls correctly installed on sys-call table\n",MODNAME);

        return 0;

}


void cleanup_module(void) {

        int i;

        printk("%s: shutting down\n",MODNAME);

        unprotect_memory();
        for(i=0;i<HACKED_ENTRIES;i++){
                ((unsigned long *)the_syscall_table)[restore[i]] = the_ni_syscall;
        }
        protect_memory();
        printk("%s: sys-call table restored to its original content\n",MODNAME);

}