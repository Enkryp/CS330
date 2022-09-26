#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]){
	// Add checks for command line arguments
	int n = atoi(argv[1]);
	int x = atoi(argv[2]);
	int pipe_file_descriptors[2];
	for(int i=0;i<n;++i){

		if(pipe(pipe_file_descriptors)<0){
			fprintf(1, "Death and damnation");
			exit(1);
		}
		x += getpid();		
		fprintf(1, "%d: %d\n", getpid(),x);
		int r = fork();
		if(r==0){
			read(pipe_file_descriptors[1], &x, 1);
			close(pipe_file_descriptors[0]);
			close(pipe_file_descriptors[1]);
		}
		else{
			write(pipe_file_descriptors[1], &x, 1);
			close(pipe_file_descriptors[0]);
			close(pipe_file_descriptors[1]);
			int status;
			wait(&status);
			exit(0);
		}
	}
	exit(0);
}