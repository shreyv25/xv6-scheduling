#include "types.h"
#include "user.h"


int
main(int argc, char *argv[])
{
	int i;
	int n;
	int j = 0;
	int k;
	int retime;
	int rutime;
	int stime;
	int sum_retime = 0;
	int sum_rutime = 0;
	int sum_stime = 0;
	if (argc!=2){
		    printf(1, "Usage: sanity <n>\n");
		    exit();
		 }

	n = atoi(argv[1]);
	i = n; //unimportant
	int pid;
	for (i = 0; i < 3 * n; i++) {
		j = i % 3;
		pid = fork();
		if (pid == 0) {//child
			switch(j) {
				case 0: //CPU‐bound process (CPU):
					for (k = 0; k < 100; k++){
						for (j = 0; j < 1000000; j++){

						}
					}
					break;
				case 1: //short tasks based CPU‐bound process (S‐CPU):
					for (k = 0; k < 100; k++){
						for (j = 0; j < 1000000; j++){

						}
					}
					yield();
					break;
				case 2:// I/O bound process (IO)
					for(k = 0; k < 100; k++){
						sleep(1);
					}
					break;
			}
			exit(); // children exit here
		}
		continue; // father continues to spawn the next child
	}
	for (i = 0; i < 3 * n; i++) {
		pid = wait2(&retime, &rutime, &stime);
		printf(1, "i: %d, pid: %d, retime: %d, rutime: %d, stime: %d\n", i, pid, retime, rutime, stime);
	}
  exit();
}
