#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <sched.h>

#define PROF
#include "rtmRegion.h"

typedef unsigned long uint64_t;

#define CPUFREQ 3400000000
#define ARRAYSIZE 4*1024*1024/CASHELINESIZE //4M
#define CASHELINESIZE 64 //64 bytes

inline uint64_t rdtsc(void)
{
    unsigned a, d;
    __asm __volatile("rdtsc":"=a"(a), "=d"(d));
    return ((uint64_t)a) | (((uint64_t) d) << 32);
}


struct Cacheline {
	char data[CASHELINESIZE];
};

//critical data
char padding0[64];
//only used in mix model
int readset = 16*1024;
int writeset = 16*1024;
char padding[64];
int workingset = 16 * 1024; //Default ws: 16KB
__thread Cacheline *array;
char padding1[64];
uint64_t cycles = CPUFREQ/1000; //1ms

volatile int ready = 0;
volatile int epoch = 0;
int thrnum = 1;
int bench = 1; // 1: read 2: write 3: mix

inline void ExecNull(){
	
	register uint64_t interval = cycles;
	register uint64_t start = rdtsc();
	while(rdtsc() - start < interval);

}


inline int Read(char * data) {
	int res = 0;
	for(int i = 0; i < workingset; i++) {
		res += (int)data[i];
	}
	return res;
}

inline void Write(char * data) {
	register int ws = workingset;
	for(int i = 0; i < ws; i++) {
		data[i]++;
	}
}


inline int WriteRead(char* data) {
	int res = 0;
	int i = 0;

	register int ws = writeset;
	
	for(; i < ws; i++) {
			data[i]++;
	}	

	int j = i;
	for(; i < readset + j; i++) {
		res += (int)data[i];
	}


	return res;
}


inline int ReadWrite(char* data) {
	int res = 0;
	int i = 0;

	
	for(; i < readset; i++) {
		res += (int)data[i];
	}
	
	register int ws = writeset;

	int j = i;
	
	for(; i < ws + j; i++) {
		data[i]++;
	}


	return res;
}

int
diff_timespec(const struct timespec &end, const struct timespec &start)
{
    int diff = (end.tv_sec > start.tv_sec)?(end.tv_sec-start.tv_sec)*1000:0;
    assert(diff || end.tv_sec == start.tv_sec);
    if (end.tv_nsec > start.tv_nsec) {
        diff += (end.tv_nsec-start.tv_nsec)/1000000;
    } else {
        diff -= (start.tv_nsec-end.tv_nsec)/1000000;
    }
    return diff;
}

void thread_init(){
	//Allocate the array at heap
	array = (Cacheline *)malloc(ARRAYSIZE * sizeof(Cacheline));
	printf("Allocal array %lx\n", array);	
	//Touch every byte to avoid page fault 
	memset(array, 1, ARRAYSIZE * sizeof(Cacheline)); 

}

void* thread_body(void *x) {

	RTMRegionProfile prof;
	int count = 0;
	int lbench = bench;
	int lepoch = 0;
	
	struct timespec start, end;
	
	uint64_t tid = (uint64_t)x;
	int cpu = 1;
	if(tid == 1)
		cpu = 5;

	cpu_set_t  mask;
    	CPU_ZERO(&mask);
    	CPU_SET(cpu, &mask);
    	sched_setaffinity(0, sizeof(mask), &mask);

	thread_init();
	
	__sync_fetch_and_add(&ready, 1);
	
	while(epoch == 0);
	
	clock_gettime(CLOCK_REALTIME, &start);


	lepoch = epoch;
	
	while(true) {

		register int res = 0;
		
		{
			RTMRegion rtm(&prof);
			if(lbench == 1)
				res += Read((char *)array);
			else if(lbench == 2) {
//				count += Read((char *)array);	
				Write((char *)array);
			}
			else if(lbench == 3)
				res += ReadWrite((char *)array);
			else if(lbench == 4)
				res += WriteRead((char *)array);
			else if(lbench == 5)
				ExecNull();
		}

		count += res;
		if(lepoch < epoch) {



			clock_gettime(CLOCK_REALTIME, &end);
			printf("Thread [%d] Time: %.2f s \n", 
						tid, diff_timespec(end, start)/1000.0);

			prof.ReportProfile();
			prof.Reset();
			count += res;
			printf("count %d\n", count);
		
			clock_gettime(CLOCK_REALTIME, &start);


			lepoch = epoch;
			
		}

	}

	prof.ReportProfile();
	
}


int main(int argc, char** argv) {

	//Parse args
	for(int i = 1; i < argc; i++) {
		int n = 0;
		char junk;
		if (strcmp(argv[i], "--help") == 0){
			printf("./a.out --ws=working set size (KB default:16KB)\n");
					return 1;
		}
		else if(sscanf(argv[i], "-ws=%d%c", &n, &junk) == 1) {
			workingset = n * 1024;
		}else if(sscanf(argv[i], "-wset=%d%c", &n, &junk) == 1) {
			writeset = n * 1024;
		}else if(sscanf(argv[i], "-rset=%d%c", &n, &junk) == 1) {
			readset= n * 1024;
		}else if(sscanf(argv[i], "-cycle=%d%c", &n, &junk) == 1) {
			cycles= n * 3400;
		}else if(strcmp(argv[i], "-ht") == 0) {
			thrnum = 2;
		}else if(strcmp(argv[i], "-r") == 0) {
			bench = 1;
		}else if(strcmp(argv[i], "-w") == 0) {
			bench = 2;
		}else if(strcmp(argv[i], "-rw") == 0) {
			bench = 3;
		}else if(strcmp(argv[i], "-wr") == 0) {
			bench = 4;
		}else if(strcmp(argv[i], "-c") == 0) {
			bench = 5;
		}
	}

	if(bench == 1 || bench == 2)
		printf("Touch Work Set %d\n", workingset);
	else if(bench == 3)
		printf("Read %d Write %d\n", readset, writeset);
	else if(bench == 5)
		printf("TX exec time %d us\n", cycles / 3400);
	pthread_t th[2];
	for(int i = 0; i < thrnum; i++)
		pthread_create(&th[i], NULL, thread_body, (void *)i);

	//Barriar to wait all threads become ready
	while (ready < 1);

	//Begin at the first epoch
	epoch = 1;

	sleep(1); //for warmup
	epoch++;
	
	while(true) {
		sleep(5);
		epoch++;
	}

	for(int i = 0; i < thrnum; i++)
		pthread_join(th[i], NULL);
	
	
	return 1;
}

