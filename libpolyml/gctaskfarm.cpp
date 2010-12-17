/*
    Title:      Task farm for Multi-Threaded Garbage Collector

    Copyright (c) 2010 David C. J. Matthews

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.
    
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(WIN32)
#include "winconfig.h"
#else
#error "No configuration file"
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef HAVE_ASSERT_H
#include <assert.h>
#define ASSERT(x)   assert(x)
#else
#define ASSERT(x)
#endif

#if (defined(WIN32) && defined(_DEBUG))
#include <stdio.h>
#endif

#include "gctaskfarm.h"

GCTaskFarm::GCTaskFarm()
{
    queueSize = queueIn = queuedItems = 0;
    workQueue = 0;
    terminate = false;
    threadCount = activeThreadCount = 0;
}

GCTaskFarm::~GCTaskFarm()
{
    Terminate();
    free(workQueue);
}

void GCTaskFarm::DebugOutput(const char *debug)
{
#if (defined(WIN32) && defined(_DEBUG))
    char debugBuff[1000];
    sprintf(debugBuff, "%u: %u: %s %u\r\n", GetTickCount(), GetCurrentThreadId(), debug, queuedItems);
    OutputDebugString(debugBuff);
#endif
}


bool GCTaskFarm::Initialise(unsigned thrdCount, unsigned qSize)
{
    terminate = false;
    if (!waitForWork.Init(0, thrdCount)) return false;
    workQueue = (queue_entry*)calloc(qSize, sizeof(queue_entry));
    if (workQueue == 0) return false;
    queueSize = qSize;

    // Create the worker threads.
    for (unsigned i = 0; i < thrdCount; i++) {
        // Fork a thread
#if ((!defined(WIN32) || defined(__CYGWIN__)) && defined(HAVE_PTHREAD_H))
        // Create a thread that isn't joinable since we don't want to wait
        // for it to finish.
        pthread_attr_t attrs;
        pthread_attr_init(&attrs);
        pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);
        pthread_t pthreadId;
        bool isError = pthread_create(&pthreadId, &attrs, WorkerThreadFunction, this) != 0;
        pthread_attr_destroy(&attrs);
        if (isError) break;
#elif defined(HAVE_WINDOWS_H)
        DWORD dwThrdId; // Have to provide this although we don't use it.
        HANDLE threadHandle =
            CreateThread(NULL, 0, WorkerThreadFunction, this, 0, &dwThrdId);
        if (threadHandle == NULL) break;
        CloseHandle(threadHandle); // Not required
#endif
        threadCount++;
    }

    return true;
}

void GCTaskFarm::Terminate()
{
    terminate = true;
    // Increment the semaphore by the number of threads to release them all.
    for (unsigned i = 0; i < threadCount; i++) waitForWork.Signal();
}

// Add work to the queue.  Returns true if it succeeds.
bool GCTaskFarm::AddWork(gctask work, void *arg1, void *arg2)
{
    PLocker l(&workLock);
    if (queuedItems == queueSize) return false; // Queue is full
    DebugOutput("Adding work");
    workQueue[queueIn].task = work;
    workQueue[queueIn].arg1 = arg1;
    workQueue[queueIn].arg2 = arg2;
    queueIn++;
    if (queueIn == queueSize) queueIn = 0;
    queuedItems++;
    if (queuedItems < threadCount) waitForWork.Signal();
    return true;
}

// Schedule this as a task or run it immediately if the queue is full.
void GCTaskFarm::AddWorkOrRunNow(gctask work, void *arg1, void *arg2)
{
    if (! AddWork(work, arg1, arg2))
        (*work)(arg1, arg2);
}

void GCTaskFarm::ThreadFunction()
{
    workLock.Lock();
    activeThreadCount++;
    while (! terminate) {
        // Invariant: We have the lock and the activeThreadCount includes this thread.
        // Find some work.

        if (queuedItems > 0) { // There is work
            unsigned outPos;
            if (queuedItems > queueIn)
                outPos = queueIn+queueSize-queuedItems;
            else outPos = queueIn-queuedItems;
            gctask work = workQueue[outPos].task;
            void *arg1 = workQueue[outPos].arg1;
            void *arg2 = workQueue[outPos].arg2;
            workQueue[outPos].task = 0;
            queuedItems--;
            DebugOutput("Found work");
            ASSERT(work != 0);
            workLock.Unlock();
            (*work)(arg1, arg2);
            workLock.Lock();
        }
        else {
            activeThreadCount--; // We're no longer active
            // If there is no work and we're the last active thread signal the
            // main thread that the queue is empty
            if (activeThreadCount == 0)
                waitForCompletion.Signal();
            // Now release the lock.
            workLock.Unlock();

            if (terminate) return;
            // Block until there's work.
            DebugOutput("Worker thread blocking");
            waitForWork.Wait();
            // We've been woken up
            workLock.Lock();
            activeThreadCount++;
        }
    }
}

#ifdef HAVE_PTHREAD_H
void *GCTaskFarm::WorkerThreadFunction(void *parameter)
#else
DWORD WINAPI GCTaskFarm::WorkerThreadFunction(void *parameter)
#endif
{
    GCTaskFarm *t = (GCTaskFarm *)parameter;
    t->ThreadFunction();
    return 0;
}

// Wait until the queue is empty.
void GCTaskFarm::WaitForCompletion(void)
{
    DebugOutput("Waiting for completion");
    workLock.Lock();
    while (activeThreadCount > 0 || queuedItems > 0)
        waitForCompletion.Wait(&workLock);
    workLock.Unlock();
    DebugOutput("Completed");
}