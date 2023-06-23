typedef struct queue{
	int rear;
	int front;
  int index_list[64];
  int size;
}Queue;

Queue mlfq[3];

int enqueue(int level, int index);
int dequeue(int level);
int is_full(int level);
int is_empty(int level);
void qinit();