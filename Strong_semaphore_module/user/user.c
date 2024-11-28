#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/sem.h>

union semun {
	int              val;    /* Value for SETVAL */
	struct semid_ds *buf;    /* Buffer for IPC_STAT, IPC_SET */
	unsigned short  *array;  /* Array for GETALL, SETALL */
	struct seminfo  *__buf;  /* Buffer for IPC_INFO
								(Linux-specific) */
};

void* do_job(void * args){

	struct sembuf op;

	printf("%s\n", (char *)args);

    int syscall_num = atoi(strtok(args, " "));
    int sem_ds = atoi(strtok(NULL, " "));
    int tokens = atoi(strtok(NULL, " "));
	int linux_sem_ds = atoi(strtok(NULL, " "));
	int index = atoi(strtok(NULL, " "));
    
    int res = -1;
	res = syscall(syscall_num, sem_ds, tokens);
	printf("%d: sys call %d returned value %d\n", index, syscall_num, res);

	op.sem_num = 0;
	op.sem_op = 1;
	op.sem_flg = 0;

	if(semop(linux_sem_ds, &op, 1) == -1) {
		printf("Error in linux semaphore lock. \n");
		return NULL;
	}

	return NULL;
}

int main(int argc, char** argv){
	
	int num_threads, op_sem_syscall, tokens, sem_ds;	
	pthread_t tid;
	struct sembuf op;
	int i;

	if(argc < 5){
		printf("usage: prog num-spawns semaphore-descriptor lock/unlock_semaphore_syscall-num tokens_num\n");
		return EXIT_FAILURE;
	}
	
	num_threads = strtol(argv[1],NULL,10);
	sem_ds = strtol(argv[2],NULL,10);
    op_sem_syscall = strtol(argv[3], NULL, 10);
    tokens = strtol(argv[4], NULL, 10);

	int linux_sem_ds = semget(IPC_PRIVATE, 1, IPC_CREAT | IPC_EXCL | 0666);

	if(linux_sem_ds == -1) {
		printf("Main - Error in semget. \n");
		return 1;
	}

	union semun sem_arg;
	sem_arg.val = 0;

	if(semctl(linux_sem_ds, 0, SETVAL, sem_arg) == -1) {
		printf("Main - Error in semaphore inizialitation. \n");
		return 1;
	}

	char args[num_threads][15];

	for (i=0; i<num_threads; i++){
		sprintf(args[i], "%d %d %d %d %d", op_sem_syscall, sem_ds, tokens, linux_sem_ds, i);
		pthread_create(&tid,NULL,do_job,(void*)args[i]);
	}

	op.sem_num = 0;
	op.sem_op = -num_threads;
	op.sem_flg = 0;

	if(semop(linux_sem_ds, &op, 1) == -1) {
		printf("Main - Error in linux semaphore lock. \n");
		return 1;
	}

	return 0;
}