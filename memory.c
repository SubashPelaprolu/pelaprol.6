#include <stdlib.h>
#include <strings.h>
#include "memory.h"

void ft_clean(struct frame_table * ft, const int f){
	struct frame *fr = &ft->frames[f];
  fr->page_index = fr->user_index = -1;
	fr->flags = 0;

  int n = f / 8;
	int bit = f % 8;
  ft->bitmap[n] &= ~(1 << bit);
}

int ft_find_unallocated(struct frame_table * ft){
	int i;
  for(i=0; i < FRAMES_LIMIT; i++){

    int n = i / 8;
		int bit = i % 8;

		if(	((ft->bitmap[n] & (1 << bit)) >> bit) == 0){
			ft->bitmap[n] |= (1 << bit);
      return i;
    }
  }
  return -1;
}

void memory_initialize(struct frame_table * ft){

	ft->frame_at_hand = -1;
  bzero(&ft->bitmap, sizeof(char)*((FRAMES_LIMIT / 8) + 1));

	int i;
	for(i=0; i < FRAMES_LIMIT; i++){
		ft_clean(ft, i);
	}
}

void pages_initialize(struct page * pages){
	int i;
	for(i=0; i < PAGES_LIMIT; i++){
		pages[i].frame_index = -1;
		pages[i].flags = 0;
		pages[i].weight =  1.0f / (float)(i + 1);
	}
}

int random_weight_page(struct page * pages){
  int i, page=0;
  const float max_weight = pages[PAGES_LIMIT-1].weight;

  const float searched = ((float) rand() / (float) RAND_MAX) * max_weight;

  for(i=0; i < PAGES_LIMIT; i++){
    if(pages[i].weight > searched){
      page = i;
      break;
    }
  }

  for(i=1; i < PAGES_LIMIT; i++){
     pages[i].weight += pages[i-1].weight;
  }

  return page;
}
