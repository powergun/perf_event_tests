#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>

#include <fcntl.h>

#include "shm.h"
#include "syscall.h"
#include "../include/perf_event.h"
#include "../include/perf_helpers.h"
#include "../include/instructions_testcode.h"

int user_set_seed;
int page_size=4096;

struct shm_s *shm;

char *page_rand;

#define NUM_EVENTS 1024

struct event_data_t{
	int active;
	int fd;
	struct perf_event_attr attr;
	pid_t pid;
	int cpu;
	int group_fd;
	unsigned long flags;
	int read_size;
	int number_in_group;
	struct sigaction sa;
	char *mmap;
	int mmap_size;
} event_data[NUM_EVENTS];

static struct sigaction sigio;

extern struct syscall syscall_perf_event_open;

static int active_events=0;

static int find_random_active_event(void) {

	int i,x,j=0;

	if (active_events<1) return -1;

	x=rand()%active_events;

	for(i=0;i<NUM_EVENTS;i++) {
		if (event_data[i].active) {
			if (j==x) break;
			j++;
		}
	}
	return i;
}


static int lookup_event(int fd) {

	int i;

	if (active_events<1) return -1;

	for(i=0;i<NUM_EVENTS;i++) {
		if (event_data[i].active) {
			if (event_data[i].fd==fd) return i;
		}
	}
	return -1;
}


static int find_empty_event(void) {

	int i;

	for(i=0;i<NUM_EVENTS;i++) {
		if (!event_data[i].active) {
			active_events++;
			return i;
		}
	}
	return -1;

}

#if 0
static int find_random_event(void) {

	return rand()%NUM_EVENTS;

}
#endif

static int next_overflow_refresh=0;
static int next_refresh=0;

static long long prev_head=0;


long long perf_mmap_read( void *our_mmap,
				int mmap_size,
				long long prev_head) {

   struct perf_event_mmap_page *control_page = our_mmap;
   long long head;

   if (!our_mmap) return 0;

   if (mmap_size==0) return 0;

   if (control_page==NULL) {
      return -1;
   }
   head=control_page->data_head;
   rmb(); /* Must always follow read of data_head */

   /* Mark all as read */
   control_page->data_tail=head;

   return head;

}


static void our_handler(int signum, siginfo_t *info, void *uc) {

	int fd = info->si_fd;
	int i;
	int ret;
	char string[BUFSIZ];


	sprintf(string,"OVERFLOW: fd=%d\n",fd);
	write(1,string,strlen(string));

	i=lookup_event(fd);

	if (i>=0) {

	   prev_head=perf_mmap_read(event_data[i].mmap,
					event_data[i].mmap_size,
					prev_head);
        }

	/* cannot call rand() from signal handler! */
	/* we re-enter and get stuck in a futex :( */
	if (next_overflow_refresh) {
		sprintf(string,"OVERFLOW REFRESH: %d\n",next_refresh);
		write(1,string,strlen(string));
		ret=ioctl(fd, PERF_EVENT_IOC_REFRESH,next_refresh);
	}

	(void) ret;

}


static void sigio_handler(int signum, siginfo_t *info, void *uc) {

	int fd = info->si_fd;
	int i;
	int ret;
	char string[BUFSIZ];


	sprintf(string,"SIGIO: fd=%d\n",fd);
	write(1,string,strlen(string));

	i=lookup_event(fd);

	if (i>=0) {

	   prev_head=perf_mmap_read(event_data[i].mmap,
					event_data[i].mmap_size,
					prev_head);
        }

	(void) ret;

}


static int rand_refresh(void) {

	int refresh;

	switch(rand()%6) {
		case 0:	refresh=0;	break;
		case 1: refresh=1;	break;
		case 2: refresh=-1;	break;
		case 3:	refresh=rand()%100; break;
		case 4: refresh=rand();	break;
		default: refresh=1;
	}

	return refresh;

}

static int rand_period(void) {

	int period;

	switch(rand()%6) {
		case 0:	period=0;	break;
		case 1: period=1;	break;
		case 2: period=-1;	break;
		case 3:	period=rand()%100000; break;
		case 4: period=rand();	break;
		default: period=1;
	}

	return period;
}

static void open_random_event(void) {

	int fd;

	int i,read_size=0;

	i=find_empty_event();

	/* return if no free events */
	if (i<0) return;

	while(1) {
		syscall_perf_event_open.sanitise(0);
		memcpy(&event_data[i].attr,
			(struct perf_event_attr *)shm->a1[0],
			sizeof(struct perf_event_attr));
		event_data[i].pid=shm->a2[0];
		event_data[i].cpu=shm->a3[0];
		event_data[i].group_fd=shm->a4[0];
		event_data[i].flags=shm->a5[0];

#if 0
		printf("Trying pid=%d cpu=%d group_fd=%d flags=%lx\n",
			event_data[i].pid,
			event_data[i].cpu,
			event_data[i].group_fd,
			event_data[i].flags);
#endif
		fd=perf_event_open(&event_data[i].attr,
			event_data[i].pid,
			event_data[i].cpu,
			event_data[i].group_fd,
			event_data[i].flags);

		if (fd>0) break;

		/* too many open files */
		if (errno==EMFILE) return;

	}
	printf("Opening fd=%d Active=%d\n",fd,active_events);
	event_data[i].fd=fd;
	event_data[i].active=1;

	/* Setup mmap buffer */

	switch(rand()%3) {
		case 0:	event_data[i].mmap_size=(rand()%64)*getpagesize();
			break;
		case 1: event_data[i].mmap_size=
				(1 + (1<<rand()%10) )*getpagesize();
			break;
		default: event_data[i].mmap_size=rand()%65535;
	}

	event_data[i].mmap=NULL;
	event_data[i].mmap=mmap(NULL, event_data[i].mmap_size,
		PROT_READ|PROT_WRITE, MAP_SHARED, event_data[i].fd, 0);
	if (event_data[i].mmap==MAP_FAILED) event_data[i].mmap=NULL;


	/* Setup overflow? */
	if (rand()%2) {

		memset(&event_data[i].sa, 0, sizeof(struct sigaction));
		event_data[i].sa.sa_sigaction = our_handler;
		event_data[i].sa.sa_flags = SA_SIGINFO;

		if (sigaction( SIGRTMIN+2, &event_data[i].sa, NULL) < 0) {
			printf("Error setting up signal handler\n");
     		}

		fcntl(event_data[i].fd, F_SETFL, O_RDWR|O_NONBLOCK|O_ASYNC);
		fcntl(event_data[i].fd, F_SETSIG, SIGRTMIN+2);
		fcntl(event_data[i].fd, F_SETOWN,getpid());

	}


	event_data[i].number_in_group=1;

	/* Set nominal read size */
	if (event_data[i].attr.read_format&PERF_FORMAT_GROUP) {
		read_size=1;
		if (event_data[i].attr.read_format&PERF_FORMAT_TOTAL_TIME_ENABLED) read_size++;
		if (event_data[i].attr.read_format&PERF_FORMAT_TOTAL_TIME_RUNNING) read_size++;
		read_size+=(1+!!(event_data[i].attr.read_format&PERF_FORMAT_ID)) * event_data[i].number_in_group;
	}
	else {
		read_size=1; /* The value */
		if (event_data[i].attr.read_format&PERF_FORMAT_TOTAL_TIME_ENABLED) read_size++;
		if (event_data[i].attr.read_format&PERF_FORMAT_TOTAL_TIME_RUNNING) read_size++;
		if (event_data[i].attr.read_format&PERF_FORMAT_ID) read_size++;
	}
	event_data[i].read_size=read_size*sizeof(long long);
}

static void close_random_event(void) {

	int i;

	i=find_random_active_event();

	/* Exit if no events */
	if (i<0) return;

	active_events--;
	printf("Closing %d, Active=%d\n",
		event_data[i].fd,active_events);
	if (event_data[i].mmap) munmap(event_data[i].mmap,
				event_data[i].mmap_size);
	close(event_data[i].fd);
	event_data[i].active=0;
}

static void ioctl_random_event(void) {

	int i,arg,arg2,result;

	i=find_random_active_event();

	/* Exit if no events */
	if (i<0) return;

	switch(rand()%8) {
		case 0:
			printf("IOCTL ENABLE fd %d\n",event_data[i].fd);
			result=ioctl(event_data[i].fd,PERF_EVENT_IOC_ENABLE,0);
			break;
		case 1:
			printf("IOCTL DISABLE fd %d\n",event_data[i].fd);
			result=ioctl(event_data[i].fd,PERF_EVENT_IOC_DISABLE,0);
			break;
		case 2: arg=rand_refresh();
			printf("IOCTL REFRESH fd %d %d\n",event_data[i].fd,arg);
			result=ioctl(event_data[i].fd,PERF_EVENT_IOC_REFRESH,arg);
			break;
		case 3:
			printf("IOCTL RESET fd %d\n",event_data[i].fd);
			result=ioctl(event_data[i].fd,PERF_EVENT_IOC_RESET,0);
			break;
		case 4: arg=rand_period();
			printf("IOCTL PERIOD fd %d %d\n",event_data[i].fd,arg);
			result=ioctl(event_data[i].fd,PERF_EVENT_IOC_PERIOD,arg);
			break;
		case 5: arg=event_data[find_random_active_event()].fd;
			printf("IOCTL SET_OUTPUT fd %d %d\n",event_data[i].fd,arg);
			result=ioctl(event_data[i].fd,PERF_EVENT_IOC_SET_OUTPUT,arg);
			break;
		case 6: arg=rand();
			printf("IOCTL SET_FILTER fd %d %d\n",event_data[i].fd,arg);
			/* FIXME -- read filters from file */
			/* under debugfs tracing/events/ * / * /id */
			result=ioctl(event_data[i].fd,PERF_EVENT_IOC_SET_FILTER,arg);
			break;
		default:
			arg=rand(); arg2=rand();
			result=ioctl(event_data[i].fd,arg,arg2);
			printf("IOCTL RANDOM fd %d %x %x\n",
				event_data[i].fd,arg,arg2);
			break;
	}
	printf("IOCTL RESULT %d %s\n",result,result<0?strerror(errno):"OK");

}

static void prctl_random_event(void) {



}

#define MAX_READ_SIZE 65536

static long long data[MAX_READ_SIZE];


static void read_random_event(void) {

	int i,result,read_size;

	i=find_random_active_event();

	/* Exit if no events */
	if (i<0) return;

	/* Exit if event has fd of 0, not want to read stdin */
	if (event_data[i].fd==0) return;

	switch (rand()%4) {
		case 0:	read_size=event_data[i].read_size;
			break;
		case 1: read_size=(rand()%8)*sizeof(long long);
			break;
		case 2: read_size=(rand()%MAX_READ_SIZE);
			break;
		default: read_size=(rand()%MAX_READ_SIZE)*sizeof(long long);
	}

	printf("READ: fd %d %d\n",event_data[i].fd,read_size);

	result=read(event_data[i].fd,data,read_size);

	printf("READ RESULT: result %d %s\n",result,
		result<0?strerror(errno):"OK");
	if (result>0) {
		printf("READ VALUES: ");
		for(i=0;i<result/8;i++) printf("%lld ",data[i]);
		printf("\n");
	}

}

static void write_random_event(void) {

	int i,result,write_size;

	i=find_random_active_event();

	/* Exit if no events */
	if (i<0) return;

	/* Exit if event has fd of 0, not want to read stdin */
	if (event_data[i].fd==0) return;

	switch (rand()%4) {
		case 0:	write_size=event_data[i].read_size;
			break;
		case 1: write_size=(rand()%8)*sizeof(long long);
			break;
		case 2: write_size=(rand()%MAX_READ_SIZE);
			break;
		default: write_size=(rand()%MAX_READ_SIZE)*sizeof(long long);
	}

	printf("WRITE: fd %d %d\n",event_data[i].fd,write_size);

	result=write(event_data[i].fd,data,write_size);

	printf("WRITE RESULT: result %d %s\n",result,
		result<0?strerror(errno):"OK");


}

static void access_random_file(void) {

	/* FIXME -- access perf event files under /proc and /sys */
#if 0
	/proc/sys/kernel/perf_event_paranoid
	/proc/sys/kernel/perf_event_max_sample_rate
	/proc/sys/kernel/perf_event_mlock_kb
	/sys/bus/event_source/devices/
#endif
}

static void run_a_million_instructions(void) {

	instructions_million();

}


static int already_forked=0;
static pid_t forked_pid;

static void fork_random_event(void) {

	if (already_forked) {
		printf("FORK: KILLING pid %d\n",forked_pid);
		kill(forked_pid,SIGKILL);
		already_forked=0;
	}
	else {
		printf("FORKING\n");
		forked_pid=fork();

		/* we're the child */
		if (forked_pid==0) {
			while(1) instructions_million();
		}
		already_forked=1;
	}
}


int main(int argc, char **argv) {

	int i;

	/* Set up to match trinity setup, vaguely */

	shm=calloc(1,sizeof(struct shm_s));

	page_rand = memalign(page_size, page_size * 2);
	if (!page_rand) {
		exit(EXIT_FAILURE);
	}
	memset(page_rand, 0x55, page_size);
	fprintf(stderr, "page_rand @ %p\n", page_rand);

	/* Set up SIGIO handler */
	memset(&sigio, 0, sizeof(struct sigaction));
	sigio.sa_sigaction = SIG_IGN; //sigio_handler;
	sigio.sa_flags = SA_SIGINFO;

	if (sigaction( SIGIO, &sigio, NULL) < 0) {
		printf("Error setting up SIGIO signal handler\n");
     	}


	/* Initialize */
	for(i=0;i<NUM_EVENTS;i++) {
		event_data[i].active=0;
		event_data[i].fd=0;
		event_data[i].read_size=rand();
	}

	while(1) {

		switch(rand()%9) {
			case 0:	open_random_event();
				break;
			case 1: close_random_event();
				break;
			case 2: ioctl_random_event();
				break;
			case 3: prctl_random_event();
				break;
			case 4: read_random_event();
				break;
			case 5: write_random_event();
				break;
			case 6: access_random_file();
				break;
			case 7: fork_random_event();
				break;
			default:
				run_a_million_instructions();
				break;
		}
		next_overflow_refresh=rand()%2;
		next_refresh=rand_refresh();
	}

	return 0;

}
