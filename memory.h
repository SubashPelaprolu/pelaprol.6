
//variable B - 70/30 read/write probability
#define B_MAX 70

#define PAGE_SIZE	1024
#define PAGES_LIMIT	32
#define FRAMES_LIMIT 256

#define P_INDEX(address) (address / PAGE_SIZE)
#define IS_REFERENCED(flags) (flags & FLAG_REFERENCE)
#define IS_DIRTY(flags) (flags & FLAG_DIRTY)

enum flags {FLAG_REFERENCE=1, FLAG_DIRTY=2};

struct page {
	int frame_index;
	int weight;	//for -m 1
	int flags;
};

struct frame {
	int page_index;
	int user_index;
	int flags;
};

struct frame_table {
	struct frame  frames[FRAMES_LIMIT];
	unsigned char bitmap[FRAMES_LIMIT / 8];
	unsigned int frame_at_hand;
};

enum request_rw { READING=0, WRITING};
enum request_state { ACCEPTED=0, BLOCKED, WAITING, DENIED};

struct request{
	int id;
	int addr;	//memory address
	int rw;		//read or write
	int state;	//request state
	struct timespec load_time;
};


int random_weight_page(struct page * pages);

void memory_initialize(struct frame_table * ft);
void pages_initialize(struct page * pages);

int ft_find_unallocated(struct frame_table * ft);
void ft_clean(struct frame_table * ft, const int f);
