/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include <chrono>
#include <queue>

#include "common.h"
#include "executorpool.h"
#include "executorthread.h"
#include "globaltask.h"
#include "taskqueue.h"

#include <platform/timeutils.h>

extern "C" {
    static void launch_executor_thread(void *arg) {
        ExecutorThread *executor = (ExecutorThread*) arg;
        executor->run();
    }
}

void ExecutorThread::start() {
    std::string thread_name("mc:" + getName());
    // Only permitted 15 characters of name; therefore abbreviate thread names.
    std::string worker("_worker");
    std::string::size_type pos = thread_name.find(worker);
    if (pos != std::string::npos) {
        thread_name.replace(pos, worker.size(), "");
    }
    thread_name.resize(15);
    if (cb_create_named_thread(&thread, launch_executor_thread, this, 0,
                               thread_name.c_str()) != 0) {
        std::stringstream ss;
        ss << name.c_str() << ": Initialization error!!!";
        throw std::runtime_error(ss.str().c_str());
    }

    LOG(EXTENSION_LOG_INFO, "%s: Started", name.c_str());
}

void ExecutorThread::stop(bool wait) {
    if (!wait && (state == EXECUTOR_SHUTDOWN || state == EXECUTOR_DEAD)) {
        return;
    }

    state = EXECUTOR_SHUTDOWN;

    if (!wait) {
        LOG(EXTENSION_LOG_NOTICE, "%s: Stopping", name.c_str());
        return;
    }
    cb_join_thread(thread);
    LOG(EXTENSION_LOG_NOTICE, "%s: Stopped", name.c_str());
}

void ExecutorThread::run() {
    LOG(EXTENSION_LOG_DEBUG, "Thread %s running..", getName().c_str());

    for (uint8_t tick = 1;; tick++) {
        resetCurrentTask();

        if (state != EXECUTOR_RUNNING) {
            break;
        }

        updateCurrentTime();
        if (TaskQueue *q = manager->nextTask(*this, tick)) {
            manager->startWork(taskType);
            EventuallyPersistentEngine *engine = currentTask->getEngine();

            // Not all tasks are associated with an engine, only switch
            // for those that do.
            if (engine) {
                ObjectRegistry::onSwitchThread(engine);
            }

            if (currentTask->isdead()) {
                manager->doneWork(taskType);
                manager->cancel(currentTask->uid, true);
                continue;
            }

            // Measure scheduling overhead as difference between the time
            // that the task wanted to wake up and the current time
            const ProcessClock::time_point woketime =
                    currentTask->getWaketime();
            currentTask->
            getTaskable().logQTime(currentTask->getTypeId(),
                                   getCurTime() > woketime ?
                                           getCurTime() - woketime :
                                           ProcessClock::duration::zero());
            updateTaskStart();
            rel_time_t startReltime = ep_current_time();

            const auto curTaskDescr = currentTask->getDescription();
            LOG(EXTENSION_LOG_DEBUG,
                "%s: Run task \"%.*s\" id %" PRIu64,
                getName().c_str(),
                int(curTaskDescr.size()),
                curTaskDescr.data(),
                uint64_t(currentTask->getId()));

            // Now Run the Task ....
            currentTask->setState(TASK_RUNNING, TASK_SNOOZED);
            bool again = currentTask->run();

            // Task done, log it ...
            const ProcessClock::duration runtime(ProcessClock::now() -
                                                 getTaskStart());
            currentTask->getTaskable().logRunTime(currentTask->getTypeId(),
                                                  runtime);
            currentTask->updateRuntime(runtime);

            // Check if exceeded expected duration; and if so log.
            // Note: This is done before we call onSwitchThread(NULL)
            // so the bucket name is included in the Log message.
            if (runtime > currentTask->maxExpectedDuration()) {
                auto description = currentTask->getDescription();
                LOG(EXTENSION_LOG_WARNING,
                    "Slow runtime for '%.*s' on thread %s: %s",
                    int(description.size()),
                    description.data(),
                    getName().c_str(),
                    cb::time2text(runtime).c_str());
            }

            if (engine) {
                ObjectRegistry::onSwitchThread(NULL);
            }

            addLogEntry(currentTask->getTaskable().getName() +
                        to_string(currentTask->getDescription()),
                       q->getQueueType(), runtime, startReltime,
                       (runtime > currentTask->maxExpectedDuration()));

            if (engine) {
                ObjectRegistry::onSwitchThread(engine);
            }

            // Check if task is run once or needs to be rescheduled..
            if (!again || currentTask->isdead()) {
                manager->cancel(currentTask->uid, true);
            } else {
                // if a task has not set snooze, update its waketime to now
                // before rescheduling for more accurate timing histograms
                currentTask->updateWaketimeIfLessThan(getCurTime());

                // reschedule this task back into the queue it was fetched from
                const ProcessClock::time_point new_waketime =
                        q->reschedule(currentTask);
                // record min waketime ...
                if (new_waketime < getWaketime()) {
                    setWaketime(new_waketime);
                }
                LOG(EXTENSION_LOG_DEBUG,
                    "%s: Reschedule a task"
                    " \"%.*s\" id %" PRIu64 "[%" PRIu64 " %" PRIu64 " |%" PRIu64
                    "]",
                    name.c_str(),
                    int(curTaskDescr.size()),
                    curTaskDescr.data(),
                    uint64_t(currentTask->getId()),
                    uint64_t(to_ns_since_epoch(new_waketime).count()),
                    uint64_t(to_ns_since_epoch(currentTask->getWaketime())
                                     .count()),
                    uint64_t(to_ns_since_epoch(getWaketime()).count()));
            }
            manager->doneWork(taskType);
        }
    }
    // Thread is about to terminate - disassociate it from any engine.
    ObjectRegistry::onSwitchThread(nullptr);

    state = EXECUTOR_DEAD;
}

void ExecutorThread::setCurrentTask(ExTask newTask) {
    LockHolder lh(currentTaskMutex);
    currentTask = newTask;
}

// MB-24394: reset currentTask, however we will perform the actual shared_ptr
// reset without the lock. This is because the task *can* re-enter the
// executorthread/pool code from it's destructor path, specifically if the task
// owns a VBucketPtr which is marked as "deferred-delete". Doing this std::move
// and lockless reset prevents a lock inversion.
void ExecutorThread::resetCurrentTask() {
    ExTask resetThisObject;
    {
        LockHolder lh(currentTaskMutex);
        // move currentTask so we 'steal' the pointer and ensure currentTask
        // owns nothing.
        resetThisObject = std::move(currentTask);
    }
    resetThisObject.reset();
}

cb::const_char_buffer ExecutorThread::getTaskName() {
    LockHolder lh(currentTaskMutex);
    if (currentTask) {
        return currentTask->getDescription();
    } else {
        return "Not currently running any task";
    }
}

const std::string ExecutorThread::getTaskableName() {
    LockHolder lh(currentTaskMutex);
    if (currentTask) {
        return currentTask->getTaskable().getName();
    } else {
        return std::string();
    }
}

void ExecutorThread::addLogEntry(const std::string &desc,
                                 const task_type_t taskType,
                                 const ProcessClock::duration runtime,
                                 rel_time_t t, bool isSlowJob) {
    LockHolder lh(logMutex);
    TaskLogEntry tle(desc, taskType, runtime, t);
    if (isSlowJob) {
        slowjobs.push_back(tle);
    } else {
        tasklog.push_back(tle);
    }
}

const std::string ExecutorThread::getStateName() {
    switch (state.load()) {
    case EXECUTOR_RUNNING:
        return std::string("running");
    case EXECUTOR_WAITING:
        return std::string("waiting");
    case EXECUTOR_SLEEPING:
        return std::string("sleeping");
    case EXECUTOR_SHUTDOWN:
        return std::string("shutdown");
    default:
        return std::string("dead");
    }
}
