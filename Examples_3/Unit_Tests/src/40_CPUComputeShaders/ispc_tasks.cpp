#include "../../../../Common_3/Utilities/Interfaces/IThread.h"
#include <stdlib.h>
#include <stdio.h>

struct TaskGroup {
    ThreadHandle* workers;
    int workerCount;
    
    // The current task info
    void* functionPtr;
    void* data;
    int count0, count1, count2;
};

void workerThreadFunction(void* arg) {
    TaskGroup* taskGroup = (TaskGroup*)arg;
    
    typedef void (*TaskFuncPtr)(void*, int, int, int, int, int, int, int, int, int, int);
    TaskFuncPtr func = (TaskFuncPtr)taskGroup->functionPtr;
    
    // Each thread just runs its portion once and exits
    int threadIndex = (int)(intptr_t)getCurrentThreadID() % taskGroup->workerCount;
    
    func(taskGroup->data, threadIndex, taskGroup->workerCount,
         threadIndex, taskGroup->workerCount,
         threadIndex % taskGroup->count0,
         (threadIndex / taskGroup->count0) % taskGroup->count1,
         threadIndex / (taskGroup->count0 * taskGroup->count1),
         taskGroup->count0, taskGroup->count1, taskGroup->count2);
}

extern "C" {

void* ISPCAlloc(void** handlePtr, int64_t size, int32_t alignment) {
    if (*handlePtr == NULL) {
        TaskGroup* taskGroup = (TaskGroup*)calloc(1, sizeof(TaskGroup));
        taskGroup->workerCount = (int)getNumCPUCores();
        taskGroup->workers = (ThreadHandle*)calloc(taskGroup->workerCount, sizeof(ThreadHandle));
        *handlePtr = taskGroup;
    }
    
    void* ptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
    return ptr;
}

void ISPCLaunch(void** handlePtr, void* f, void* data, int count0, int count1, int count2) {
    TaskGroup* taskGroup = (TaskGroup*)*handlePtr;
    
    // Store task info
    taskGroup->functionPtr = f;
    taskGroup->data = data;
    taskGroup->count0 = count0;
    taskGroup->count1 = count1;
    taskGroup->count2 = count2;
    
    // Launch threads
    for (int i = 0; i < taskGroup->workerCount; i++) {
        ThreadDesc threadDesc = {};
        threadDesc.pFunc = workerThreadFunction;
        threadDesc.pData = taskGroup;
        snprintf(threadDesc.mThreadName, MAX_THREAD_NAME_LENGTH, "ISPCWorker%d", i);
        initThread(&threadDesc, &taskGroup->workers[i]);
    }
}

void ISPCSync(void* handle) {
    if (!handle) return;
    
    TaskGroup* taskGroup = (TaskGroup*)handle;
    
    // Wait for all threads
    for (int i = 0; i < taskGroup->workerCount; i++) {
        joinThread(taskGroup->workers[i]);
    }
    
    // Free task group
    free(taskGroup->workers);
    free(taskGroup);
}

}