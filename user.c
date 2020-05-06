#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "common.h"

int main(const int argc, char * argv[]){

	if(argc != 3){
		fprintf(stderr, "Error: Invalid number of args\n");
		return EXIT_FAILURE;
	}

	const int ui = atoi(argv[1]);
	const int m  = atoi(argv[2]);

	OSS *ossaddr = oss_init(0);
	if(ossaddr == NULL){
		return -1;
	}
	struct user * usr = &ossaddr->users[ui];

	srand(getpid());

	sem_wait(&ossaddr->mutex);
	const unsigned int life = ossaddr->clock.tv_sec + USER_LIFE;
	sem_post(&ossaddr->mutex);

	int term = 0;
	struct msgbuf msg;

	while(term == 0){

		//check if we should terminated
		sem_wait(&ossaddr->mutex);
		const int state = (life > ossaddr->clock.tv_sec) ? usr->state : USER_TERMINATE;
		sem_post(&ossaddr->mutex);

		if(state == USER_TERMINATE)
			break;

		const int prob = rand() % 100;
		usr->request.rw = (	prob < B_MAX) ? READING : WRITING;
		int page = (m == 0) ? rand() % PAGES_LIMIT : random_weight_page(usr->page);
		int offset = rand() % PAGE_SIZE;
		usr->request.addr = (page * 1024) + offset;
		usr->request.state	= WAITING;


		msg.mtype = getppid();
		msg.val = ui;
		if(	(msg_snd(&msg) == -1) ||	/* tell OSS we have a request */
				(msg_rcv(&msg) == -1)){		/* wait for reply */
			/* in case or error */
			break;	/* stop */
		}

		/* check what happened with our request */
		switch(usr->request.state){
			case ACCEPTED:
				break;
			case DENIED:	//we get here only when a deadlock has occured
			case BLOCKED:
				term = -1;
				break;

			default:
				fprintf(stderr, "CHILD: Invalid request state %d\n", usr->request.state);
				term = -1;
				break;
		}
		bzero(&usr->request, sizeof(struct request));
  }

	sem_wait(&ossaddr->mutex);
	usr->state = USER_EXITED;
	sem_post(&ossaddr->mutex);

	oss_deinit(0);
  return 0;
}
