#include "TAPSimpleEvent.h"

#include <errno.h>

TAPSimpleEvent::TAPSimpleEvent()
{
    bIsSignal = false;
	pthread_cond_init( &cond, NULL);
	pthread_mutex_init( &mutex, NULL);
}

TAPSimpleEvent::~TAPSimpleEvent()
{
	pthread_cond_destroy( &cond);
	pthread_mutex_destroy( &mutex);
}


void TAPSimpleEvent::SignalEvent()
{
    pthread_mutex_lock(&mutex);
	bIsSignal =true;
	pthread_cond_signal( &cond);
    pthread_mutex_unlock(&mutex);
}

void TAPSimpleEvent::WaitEvent()
{
    pthread_mutex_lock(&mutex);
    bIsSignal = false;
    while(!bIsSignal){
		pthread_cond_wait( &cond, &mutex);
    }
    pthread_mutex_unlock(&mutex);
}
