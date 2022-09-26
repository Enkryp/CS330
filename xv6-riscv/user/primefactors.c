#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]){
	int primes[]={2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,73,79,83,89,97};
	int prime_index = 0;
	int n = atoi(argv[1]);
	int pipefd[2];
	while(n!=1){
		if(pipe(pipefd)<0){
			fprintf(1, "Death and damnation\n");
			exit(1);
		}
		
		
			int q = 0;
			while(n%primes[prime_index]==0){
				fprintf(1,"%d, ",primes[prime_index]);
				n = n/primes[prime_index];
				++q;
			}
			if(q!=0)
				fprintf(1,"[%d]\n",getpid());
			write(pipefd[1], &n, sizeof(int));
			close(pipefd[1]);
			int r;
			if(n!=1) r = fork();
			else {
				close(pipefd[0]);
				exit(0);
			}
		if(r!=0){
			int status;
			wait(&status);
			exit(0);
		}
		else{
			read(pipefd[0], &n, sizeof(int));
			prime_index++;
			close(pipefd[0]);
		}

	}
	fprintf(1,"\n");
	exit(0);
}