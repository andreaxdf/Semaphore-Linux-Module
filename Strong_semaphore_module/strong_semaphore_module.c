
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

#define MODNAME "STRONG_SYNCRONIZATION"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrea De Filippis");
MODULE_DESCRIPTION("strong sync service");

#define AUDIT if(1)

#define NO (0)
#define YES (NO+1)

unsigned long the_syscall_table = 0x0;
module_param(the_syscall_table, ulong, 0660);

unsigned long new_sys_call_array[] = {0x0,0x0,0x0};//It will set to the three syscall at startup

unsigned long the_ni_syscall;

static int enable_sleep = 1; // this can be configured at run time via the sys file system - 1 meas any sleeping thread is freezed
module_param(enable_sleep,int,0660);

typedef struct _elem{
        struct task_struct *task;
        int pid;
        int awake;
        int requested_tokens;
        struct _elem * next;
        struct _elem * prev;
} elem;

typedef struct _waiting_list {
        elem *head;
        elem *tail;
        spinlock_t *thread_list_lock;
        struct wait_queue_head *wait_queue;
} waiting_list_t;

typedef struct _semaphore {
        int sem_ds; // Semaphore "descriptor"
        int tokens; // Semaphore tokens
        spinlock_t *sem_lock;
        waiting_list_t *waiting_list;
        struct _semaphore *next; 
} semaphore;

// Could be used a tree for a better implementation 
semaphore semaphore_list_head = {-1,-1,NULL,NULL,NULL};
semaphore *semaphore_list_tail = &semaphore_list_head; // This pointer will point at the last semaphore in the list so that the creation of a new semaphore 
int last_descriptor = -1;
spinlock_t semaphore_list_lock;

waiting_list_t *getNewWaitingList(void) {

        spinlock_t *spin;
        elem *head;
        elem *tail;
        struct wait_queue_head *queue;
        waiting_list_t *waiting_list;

        spin = (spinlock_t *)kmalloc(sizeof(spinlock_t), GFP_KERNEL);
        queue = (struct wait_queue_head *)kmalloc(sizeof(struct wait_queue_head), GFP_KERNEL);
        head = (elem *) kmalloc(sizeof(elem), GFP_KERNEL);
        tail = (elem *) kmalloc(sizeof(elem), GFP_KERNEL);

        if(head == NULL || tail == NULL || spin == NULL || queue == NULL) {
                AUDIT
                printk("%s: unable to allocate memory with kmalloc for create a new waiting list",MODNAME);
                return NULL;
        }

        *head = (elem) {NULL,-1,-1,-1,NULL,NULL};
        *tail = (elem) {NULL,-1,-1,-1,NULL,NULL};
        head->next = tail;
        tail->prev = head;

        spin_lock_init(spin);
        init_waitqueue_head(queue);

        waiting_list = (waiting_list_t *) kmalloc(sizeof(waiting_list_t), GFP_KERNEL);

        *waiting_list = (waiting_list_t) {head, tail, spin, queue};

        return waiting_list;
}

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

inline int appendToList(volatile elem *me, elem *list_tail, spinlock_t *spinlock) {

        elem *aux;

        preempt_disable();//this is redundant here
        spin_lock(spinlock);

        aux = list_tail;
        if(aux->prev == NULL){
                spin_unlock(spinlock);
                preempt_enable();
                printk("%s: malformed sleep-wakeup-queue - service damaged\n",MODNAME);
                return -1;
        }


        //TODO ha senso fare il cast?
        aux->prev->next = (elem *) me;
        me->prev = aux->prev;
        aux->prev = (elem *)me;
        me->next = aux;

        spin_unlock(spinlock);
        preempt_enable();//this is redundant here

        return 0;
}

inline void removeFromList(volatile elem *me, spinlock_t *spinlock) {
        preempt_disable();
        spin_lock(spinlock);

        me->prev->next = me->next;//we know where we are thanks to double linkage
        me->next->prev = me->prev;

        spin_unlock(spinlock);
        preempt_enable();
}

int goto_sleep(semaphore *sem, int tokens){

        volatile elem me;
        DECLARE_WAIT_QUEUE_HEAD(the_queue);//here we use a private queue - wakeup is selective via wake_up_process

        me.next = NULL;
        me.prev = NULL;
        me.task = current;
        me.pid  = current->pid;
        me.awake = NO;
        me.requested_tokens = tokens;

        if(!enable_sleep){
                printk("%s: sys_goto_sleep - sleeping not currently enabled\n",MODNAME);
                return -1;
        }

        if(appendToList(&me, sem->waiting_list->tail, sem->waiting_list->thread_list_lock) != 0) {
                return -1;
        }

        AUDIT
        printk("%s: thread %d actually going to sleep\n",MODNAME,current->pid);
        
        do {
                wait_event_interruptible(*(sem->waiting_list->wait_queue), sem->tokens >= tokens);

                AUDIT
                printk("%s: thread %d woke up. Trying to lock semaphore %d \n",MODNAME,current->pid,sem->sem_ds);
        } while(try_to_lock(sem, tokens));

        removeFromList(&me, sem->waiting_list->thread_list_lock);

        AUDIT
        printk("%s: THREAD %d FINISHED \n",MODNAME, current->pid);

        return 0;
}

int wake_up_threads(int free_tokens, waiting_list_t *waiting_list) {
        struct task_struct *the_task;
        int its_pid = -1;
        elem *curr;

        curr = waiting_list->head;

        preempt_disable();//this is redundant here
        spin_lock(waiting_list->thread_list_lock);

        if(curr == NULL){
                spin_unlock(waiting_list->thread_list_lock);
                preempt_enable();
                printk("%s: malformed sleep-wakeup-queue\n",MODNAME);
                return -1;
        }

        curr = curr->next;

        while(curr != waiting_list->tail){

                if(curr->requested_tokens <= free_tokens && curr->awake == NO){
                        the_task = curr->task;
                        curr->awake = YES;
                        its_pid = curr->pid;
                        wake_up_process(the_task);
                        free_tokens -= curr->requested_tokens;

                        AUDIT
                        printk("%s: woke up threads %d in wait queue", MODNAME, curr->pid);

                        if(free_tokens <= 0) goto token_finished;
                }

                curr = curr->next;
        }

        spin_unlock(waiting_list->thread_list_lock);
        preempt_enable();

	return 0;

token_finished:
        spin_unlock(waiting_list->thread_list_lock);
        preempt_enable();//this is redundant here

        AUDIT
        printk("%s: called the awake of thread %d\n",MODNAME,its_pid);

        return its_pid;
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
        waiting_list_t *newWaitingList;

        AUDIT
        printk("%s: creating a new semaphore",MODNAME);

        new_sem = (semaphore *)kmalloc(sizeof(semaphore), GFP_KERNEL);
        sem_lock = (spinlock_t *)kmalloc(sizeof(spinlock_t), GFP_KERNEL);
        newWaitingList = getNewWaitingList();

        if(new_sem == NULL || sem_lock == NULL) {
                AUDIT
                printk("%s: unable to allocate memory with kmalloc",MODNAME);
                return -1;
        }

        new_sem->next = NULL;
        new_sem->tokens = tokens;
        spin_lock_init(sem_lock);
        new_sem->sem_lock = sem_lock;
        new_sem->waiting_list = newWaitingList;
        
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
        printk("%s: trying to release %d tokens on semaphore %d", MODNAME, tokens, sem_ds);

        sem = getSemaphore(sem_ds);

        if(sem == NULL) return -1;

        preempt_disable();
        spin_lock(sem->sem_lock);

        sem->tokens += tokens;

        AUDIT
        printk("%s: released %d tokens on semaphore %d. Now it have %d tokens", MODNAME, tokens, sem_ds, sem->tokens);

        // We need to keep the lock because we will wake up threads which require a number of tokens <= free tokens.
        // If we release the lock, someone else can take some tokens and the threads awaken couldn't find the tokens requested. 
        wake_up_threads(sem->tokens, sem->waiting_list);

        spin_unlock(sem->sem_lock);
        preempt_enable();

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