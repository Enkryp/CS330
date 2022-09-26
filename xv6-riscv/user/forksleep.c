#include "kernel/types.h"
#include "user/user.h"

int main(int argc, char *argv[]){
	if(argc != 3){
		fprintf(2, "forksleep: Incorrect number of arguments\n");
		exit(1);
	}
	int m = atoi(argv[1]);
	if(m<=0){
		fprintf(2, "forksleep: First argument must be a postive integer\n");
		exit(1);
	}
	char n = argv[2][0];
	// if(argv[2]!="0" && argv[2]!="1"){
	// 	n = argv[2][0];
	// }
	// else{
	// 	fprintf(2, "forksleep: Second argument must be a 0 or 1\n");
	// 	exit(1);
	// }
	if(n=='0'){
		int r = fork();
		if(r==0){
			sleep(m);
			fprintf(1, "%d: Child\n", getpid());
		}
		else{
			fprintf(1, "%d: Parent\n", getpid());
			// int status;
			// wait(&status);
		}
	}
	else{
		int r = fork();
		if(r!=0){
			sleep(m);
			fprintf(1, "%d: Parent\n", getpid());

		}
		else{
			fprintf(1, "%d: Child\n", getpid());
		}
	}
	exit(0);
}