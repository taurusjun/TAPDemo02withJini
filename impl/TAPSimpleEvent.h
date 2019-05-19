#ifndef TAPSIMPLE_EVENT_H
#define TAPSIMPLE_EVENT_H

#include <pthread.h>

class TAPSimpleEvent
{

public:
	TAPSimpleEvent();
	~TAPSimpleEvent();

	void SignalEvent();
	void WaitEvent();

private:	
	pthread_cond_t cond;
	pthread_mutex_t mutex;
    bool bIsSignal;
};

#endif //TAPSIMPLE_EVENT_H
