#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
//#include <errno.h>
#include <sys/ipc.h>
#include <sys/wait.h>

#include "common.h"
#include "user.h"
#include "bb.h"

static 			 unsigned int num_users = 0;
static const unsigned int max_users = 100;
static 			 unsigned int num_exited = 0;

/* counters for request type - read and writes */
static unsigned int num_read=0, num_write=0;
static unsigned int num_fault=0, num_ref=0;
static unsigned int num_lines = 0;

static int arg_verbose = 1;
static int arg_m = 0;

static OSS *ossaddr = NULL;
static const char * logfile = "output.txt";

static int sig_flag = 0;	//raise, if we were signalled
static unsigned int user_bitmap = 0;

//Queues for processes with requests
static struct bounded_buffer req_bb; // request queue

//check which user entry is free, using bitmap
static int get_free_ui(){
	int i;
  for(i=0; i < USER_LIMIT; i++){
  	if(((user_bitmap & (1 << i)) >> i) == 0){	//if bit is 0, than means entry is free
			user_bitmap ^= (1 << i);	//set bit to 1, to mark it used
      return i;
    }
  }
  return -1;
}
//Clear a user entry
static void ui_clear(struct user * users, const unsigned int i){

	//clear pages
	int j;
	for(j=0; j < PAGES_LIMIT; j++){
		struct page *page = &users[i].page[j];
		if(page->frame_index >= 0){
			ft_clean(&ossaddr->ft, page->frame_index);
		}
	}

	pages_initialize(users[i].page);
	users[i].state = 0;

}

static struct user * ui_new(struct user * users, const int ID){
	const int i = get_free_ui();
	if(i == -1){
		return NULL;
	}

	ui_clear(users, i);

  users[i].ID	= ID;
  users[i].state = USER_READY;

	return &users[i];
}

static void ui_free(struct user * users, const unsigned int i){
	user_bitmap ^= (1 << i); //set bit to 0, to mark it free
	ui_clear(users, i);
}

static int user_fork(void){

  struct user *usr;
	char arg[100], argM[100];


  if((usr = ui_new(ossaddr->users, num_users)) == NULL)
    return 0;

	const int ui = usr - ossaddr->users; //process index
	snprintf(arg, sizeof(arg), "%u", ui);
	snprintf(argM, sizeof(argM), "%u", arg_m);

	const pid_t pid = fork();
	switch(pid){
		case -1:
			ui_free(ossaddr->users, ui);
			perror("fork");
			break;

		case 0:

			execl("./user", "./user", arg, argM, NULL);
			perror("execl");
			exit(EXIT_FAILURE);

		default:
			num_users++;
			usr->pid = pid;
			break;
  }

  printf("[%li.%li] OSS: Generating process with PID %u\n", ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, usr->ID);
	return 0;
}

static int fork_ready(struct timespec *forkat){

  if(num_users < max_users){  //if we can fork more

    // if its time to fork
    if(timercmp(&ossaddr->clock, forkat)){

      //next fork time
      forkat->tv_sec = ossaddr->clock.tv_sec + 1;
      forkat->tv_nsec = 0;

      return 1;
    }
  }
  return 0; //not time to fokk
}

//Stop users. Used only in case of an alarm.
static void stop_users(){
  int i;
  struct msgbuf mb;
	memset(&mb, 0, sizeof(mb));

	mb.val = DENIED;

  for(i=0; i < USER_LIMIT; i++){
    if(ossaddr->users[i].pid > 0){
			ossaddr->users[i].request.state = DENIED;
      mb.mtype = ossaddr->users[i].pid;
      msg_snd(&mb);
    }
  }
}

static void signal_handler(int sig){
  printf("[%li.%li] OSS: Signal %d\n", ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, sig);
  sig_flag = sig;
}

static void current_frames(){
	const char row_format[] = "%10s\t\t%10s\t\t%10d\t%10d\n";
	char buf[10+1];
	int i;

	printf("[%li.%li] OSS: Current Frames Usage:\n", ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec);
	printf("%10s\t\t%10s\t\t%10s\t%10s\n", "#", "Occupied", "RefByte", "DirtyBit");
	num_lines += 2;

	for(i=0; i < FRAMES_LIMIT; i++){

		snprintf(buf, sizeof(buf), "Frame %4d", i);

    struct frame * fr = &ossaddr->ft.frames[i];

    if(fr->page_index > 0){
			struct page * page = &ossaddr->users[fr->user_index].page[fr->page_index];
			printf(row_format, buf, "Yes", page->flags & FLAG_REFERENCE, fr->flags & FLAG_DIRTY);
    }else{
			printf(row_format, buf, "No", 0, 0);
		}
	}
	printf("\n");
  num_lines += FRAMES_LIMIT;
}

static int ft_evict_frame(struct frame_table * ft){

	int i;
  for(i=0; i < FRAMES_LIMIT; i++){

		ft->frame_at_hand++;
    ft->frame_at_hand %= FRAMES_LIMIT;
		struct frame * frame = &ft->frames[ft->frame_at_hand];

		if(frame->page_index >= 0){

			struct page * page = &ossaddr->users[frame->user_index].page[frame->page_index];
		  if(IS_REFERENCED(page->flags) == 0){

				num_lines++;
		    printf("[%li.%li] OSS: P%d page %d was evicted from frame %d\n",
		    	ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, frame->page_index, frame->user_index, ft->frame_at_hand);

				break;
			}else{
				page->flags ^= FLAG_REFERENCE;	//drop reference flag
		  }
		}
  }

  return ft->frame_at_hand;
}

static void ft_save_dirty(const int frame){
	num_lines++;
	printf("[%li.%li] OSS: Dirty bit of frame %d set, adding additional time to the clock\n",
		ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, frame);

	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 14* 1000;
	timeradd(&ossaddr->clock, &ts);
}

static int ft_swap_frame(int f, int page, struct user * usr){
	struct frame * frame = &ossaddr->ft.frames[f]; //frame that holds the evicted page
	struct page * old_page = &ossaddr->users[frame->user_index].page[frame->page_index];
	struct user * old_usr = &ossaddr->users[frame->user_index];

	num_lines++;
	printf("[%li.%li] OSS: Cleared frame %d and swap P%d page %d for P%d page %d\n",
		ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, old_page->frame_index, old_usr->ID, frame->page_index, usr->ID, page);

	f = old_page->frame_index;
	ft_clean(&ossaddr->ft, f);
	old_page->frame_index = -1;

	return f;
}

static int ft_page_fault(struct user * usr){

	num_fault++;
  const int pa = P_INDEX(usr->request.addr);

  int free_frame = ft_find_unallocated(&ossaddr->ft);
  if(free_frame >= 0){
    num_lines++;
    printf("[%li.%li] OSS: Found free frame %d for P%d page %d\n",
      ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, free_frame, usr->ID, pa);

  }else{

	  int swap_frame = ft_evict_frame(&ossaddr->ft); //choose a frame, using CLOCK
	  struct frame * frame = &ossaddr->ft.frames[swap_frame]; //frame that holds the evicted page

		if(IS_DIRTY(frame->flags)){
			ft_save_dirty(swap_frame);
		}

		free_frame = ft_swap_frame(swap_frame, pa, usr);
	}

  return free_frame;
}

static int ft_load_addr(struct user * usr){

	int rv;
  struct page * page = &usr->page[P_INDEX(usr->request.addr)];

  if(page->frame_index < 0){	//page has no frame

		num_lines++;
    printf("[%li.%li] OSS: Page fault for address %d\n",
      ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, usr->request.addr);

  	page->frame_index = ft_page_fault(usr);
  	page->flags ^= FLAG_REFERENCE;

		//"load" the page into frame
		struct frame * frame = &ossaddr->ft.frames[page->frame_index];
  	frame->page_index = P_INDEX(usr->request.addr);
  	frame->user_index = usr - ossaddr->users;

    //loading takes 14 ms
    usr->request.load_time.tv_sec = 0;
		usr->request.load_time.tv_nsec = 14 * 1000;
    timeradd(&usr->request.load_time, &ossaddr->clock);
		rv = BLOCKED;

  }else{	//page has a frame

		//add access time to clock
		struct timespec ts;
		ts.tv_sec = 0;
    ts.tv_nsec = 10;
    timeradd(&ossaddr->clock, &ts);
		rv = ACCEPTED;	//page is available to read/write
  }

  return rv;
}

static int req_process(struct user * usr){

	int rv = DENIED;

  num_ref++;

	switch(usr->request.rw){
		case READING:
			num_read++;
			printf("[%li.%li] OSS: P%d referencing address %d\n", ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, usr->ID, usr->request.addr);
			break;
		case WRITING:
			num_write++;
			printf("[%li.%li] OSS: P%d referencing address %d\n", ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, usr->ID, usr->request.addr);
			break;
		default:
			fprintf(stderr, "Error: Reference type %d unknown\n", usr->request.rw);
			return DENIED;
	}
  num_lines++;

	rv = ft_load_addr(usr);
  if(rv == ACCEPTED){
    const int pa = P_INDEX(usr->request.addr);
    struct page * page = &usr->page[pa];

		switch(usr->request.rw){
			case READING:
				num_lines++;
				printf("[%li.%li] OSS: Address %d in frame %d, P%d reading data\n", ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec,
					usr->request.addr, page->frame_index, usr->ID);
				break;

			case WRITING:
				num_lines++;
				printf("[%li.%li] OSS: Address %d in frame %d, P%d writing data\n",
					ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, usr->request.addr, page->frame_index, usr->ID);

				//after frame is written, it gets dirty
				ossaddr->ft.frames[page->frame_index].flags |= FLAG_DIRTY;
				break;

			default:
				break;
		}
  }
	return rv;
}

static int req_dispatch(){
	int i, count=0;
  struct timespec tdisp;
	struct msgbuf msg;

  tdisp.tv_sec = 0;

	const int nreq = bb_size(&req_bb);

	for(i=0; i < nreq; i++){

		const int ui = bb_pop(&req_bb);	/* get index of process who has a request */
		struct user * usr = &ossaddr->users[ui];

		if(usr->pid > 0){	//process is running

			usr->request.state = req_process(usr);

			if(usr->request.state != BLOCKED){	//if request was accepted or denied
				count++;

				//send message to user to unblock
				msg.mtype = usr->pid;
				msg.val = ui;
				if(msg_snd(&msg) == -1){
					break;
				}
			}else{
				/* push the blocked request at end of queue */
				bb_push(&req_bb, ui);
			}

    	if((arg_verbose == 1) && ((num_ref % 20) == 0))
    		current_frames();
		}

    //add request processing to clock
    tdisp.tv_nsec = rand() % 100;
    timeradd(&ossaddr->clock, &tdisp);
	}

	return count;	//return number of dispatched procs
}

static int req_gather(){
	int n = 0;	/* new request */
	struct msgbuf msg;

	usleep(20);	/* give some time of users to enqueue more requests */

	while(1){
		if(msg_rcv_nb(&msg) < 0){
			if(errno == ENOMSG){
				break;	//stop
			}else{
				return -1;	//return error
			}
		}

		if(bb_push(&req_bb, msg.val) < 0){	/* if queue is full */
			break;	//stop
		}
		n++;
	}

	if(arg_verbose && (n > 0)){
		printf("[%li.%li] Master added %d new request. Queue has %d\n", ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, n, bb_size(&req_bb));
	}
	return n;
}

static void check_exited(){
	int i;

	for(i=0; i < USER_LIMIT; i++){

		struct user * usr = &ossaddr->users[i];

		sem_wait(&ossaddr->mutex);
		if(usr->state == USER_EXITED){
			num_exited++;
			printf("[%li.%li] Master has detected P%d exited\n",
				ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec, usr->ID);

			ui_free(ossaddr->users, i);
		}
		sem_post(&ossaddr->mutex);
	}
}

void parse_args(const int argc, char * argv[]){
	int opt;
	while(( opt = getopt(argc, argv, "vm:")) > 0){
		switch(opt){
			case 'v':
				arg_verbose = 1;
				break;
			case 'm':
				arg_m = atoi(optarg);
				break;
			default:
				fprintf(stderr, "Error: Invalid option %c\n", opt);
				break;
		}
	}
}

int main(const int argc, char * argv[]){

	parse_args(argc, argv);

  signal(SIGINT,  signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGALRM, signal_handler);
  signal(SIGCHLD, SIG_IGN);

	stdout = freopen(logfile, "w", stdout);
  if(stdout == NULL){
		perror("freopen");
		return -1;
	}

  ossaddr = oss_init(1);
  if(ossaddr == NULL){
    return -1;
  }


  bb_init(&req_bb);
	memory_initialize(&ossaddr->ft);
	int i;
	for(i=0; i < USER_LIMIT; i++){
		pages_initialize(ossaddr->users[i].page);
	}
	//alarm(2);

	struct timespec inc;		//clock increment
  struct timespec forkat;	//when to fork another process

	const unsigned int ns_step = 10000;

	inc.tv_sec = 1;
	timerzero(&forkat);

  while(!sig_flag){

		if(num_exited >= max_users)
			break;

	  //increment system clock
	  inc.tv_nsec = rand() % ns_step;
	  timeradd(&ossaddr->clock, &inc);

		//if we are ready to fork, start a process
    if(fork_ready(&forkat) && (user_fork() < 0))
      break;

		//resource logic
		req_gather();
    req_dispatch();

  	if(num_lines >= 100000){
  		printf("[%li.%li] OSS: Closing output, due to line limit ...\n", ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec);
  		stdout = freopen("/dev/null", "w", stdout);
  	}

		check_exited();
  }

  printf("Runtime: %li:%li\n", ossaddr->clock.tv_sec, ossaddr->clock.tv_nsec);
	printf("Users: %d/%d\n", num_users, num_exited);
	printf("Num. references: %u\n", num_ref);
	printf("Num. read: %u\n", num_read);
	printf("Num. write: %u\n", num_write);
	printf("References/second: %.2f\n", (float) num_ref / ossaddr->clock.tv_sec);
	printf("Faults/reference: %.2f\n", (float)num_fault / num_ref);

  stop_users(-1);
  oss_deinit(1);
  return 0;
}
