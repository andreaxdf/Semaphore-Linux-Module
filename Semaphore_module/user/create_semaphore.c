#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>


int main(int argc, char** argv){
	
	int create_sem_syscall, tokens;

	if(argc < 3){
		printf("usage: create_semaphore_syscall-num tokens_num\n");
		return EXIT_FAILURE;
	}
	
	create_sem_syscall = strtol(argv[1],NULL,10);
    tokens = strtol(argv[2], NULL, 10);

    int sem_ds;

    sem_ds = syscall(create_sem_syscall, 0);

    printf("Created a semaphore with descriptor: %d and tokens: %d\n", sem_ds, tokens);
}
