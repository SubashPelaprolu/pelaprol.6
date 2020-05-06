#ifndef USER_H
#define USER_H

#include <semaphore.h>
#include "timer.h"
#include "memory.h"

//Runtime seconds for user process
#define USER_LIFE 100
#define USER_LIMIT 20

enum user_state { USER_READY=1, USER_BLOCKED, USER_TERMINATE, USER_EXITED };

struct user {
	pid_t	pid;
	int ID;

	enum user_state state;
	struct request request;

	struct page	  page[PAGES_LIMIT];	//virtual memory pages
};

#endif
