#include "types.h"
#include "defs.h"
#include "queue.h"

int enqueue(int level, int index)
{ 
	mlfq[level].rear = (mlfq[level].rear + 1) % 64;		//	rear 하나 증가시키기
	mlfq[level].index_list[mlfq[level].rear] = index; //	list의 rear에 data 추가
	mlfq[level].size++;
	return 1;
}

/*		큐 데이터 꺼내기		*/
int dequeue(int level)
{ 
	mlfq[level].front = (mlfq[level].front + 1) % 64;
	mlfq[level].size--;
	return mlfq[level].index_list[mlfq[level].front];
}

/*		공백 상태인지 여부		*/
int is_empty(int level)
{
	if (mlfq[level].front == mlfq[level].rear)
		return 1;
	else
		return 0;
}

/*		포화 상태인지 여부		*/
int is_full(int level)
{
	if (mlfq[level].size==64) 
		return 1;
	else
		return 0;
}

void qinit()
{
	for (int i = 0; i < 3; i++)
	{
		mlfq[i].front = 0;
		mlfq[i].rear = 0;
		mlfq[i].size = 0;
	}
}