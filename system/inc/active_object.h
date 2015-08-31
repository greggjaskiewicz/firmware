/**
  Copyright (c) 2015 Particle Industries, Inc.  All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, see <http://www.gnu.org/licenses/>.
  ******************************************************************************
 */


#pragma once

#if PLATFORM_THREADING

#include <functional>
#include <mutex>
#include <thread>
#include <future>
#include "channel.h"

struct ActiveObjectConfiguration
{
    /**
     * Function to call when there are no objects to process in the queue.
     */
    typedef std::function<void(void)> background_task_t;

    /**
     * The function to run when there is nothing else to do.
     */
    background_task_t background_task;
    size_t stack_size;

    /**
     * Time to wait for a message in the queue.
     */
    unsigned take_wait;


public:
    ActiveObjectConfiguration(background_task_t task, unsigned take_wait_, size_t stack_size_ =0) : background_task(task), stack_size(stack_size_), take_wait(take_wait_) {}

};

struct Message
{
    virtual void operator()()=0;
};

template<typename T> class Promise : public Message
{
    /**
     * The function to invoke to retrieve the future result.
     */
    std::function<T()> fn;
    /**
     * The result retrieved from the function.
     * Only valid once {@code complete} is {@code true}
     */
    T result;
    bool complete;

    std::mutex m;
    std::condition_variable cv;

public:

    Promise(std::function<T()>&& fn_) : fn(std::move(fn_)), complete(false)
    {
    }

    virtual void operator()() override
    {
        {
            std::lock_guard<std::mutex> lk(m);
            result = fn();
            complete = true;
        }
        cv.notify_one();
    }

    /**
     * wait for the result
     */
    T get()
    {
        // wait for result to be available
        std::lock_guard<std::mutex> lk(m);
        cv.wait(lk, [this]{return complete;});
        return result;
    }
};



class ActiveObjectBase
{
public:
    using Item = std::shared_ptr<Message>;

protected:

    ActiveObjectConfiguration configuration;

    /**
     * The thread that runs this active object.
     */
    std::thread _thread;

    std::thread::id _thread_id;

    std::mutex _start;

    volatile bool started;

    /**
     * The main run loop for an active object.
     */
    void run();

    //void invoke_impl(void* fn, void* data, size_t len=0);

protected:

    // todo - concurrent queue should be a strategy so it's pluggable without requiring inheritance
    virtual bool take(Item& item)=0;
    virtual void put(Item& item)=0;

    void set_thread(std::thread&& thread)
    {
        this->_thread.swap(thread);
        _thread_id = _thread.get_id();
    }

    /**
     * Static thread entrypoint to run this active object loop.
     * @param obj
     */
    static void run_active_object(ActiveObjectBase* obj);

    void start_thread();

public:

    ActiveObjectBase(const ActiveObjectConfiguration& config) : configuration(config), started(false) {}

    bool isCurrentThread() {
        return _thread_id == std::this_thread::get_id();
    }

    bool isStarted() {
        return started;
    }

    template<typename arg, typename r> inline void invoke(r (*fn)(arg* a), arg* value, size_t size) {
        //invoke_impl((void*)fn, value, size);
    }

    template<typename arg, typename r> inline void invoke(r (*fn)(arg* a), arg* value) {
        //invoke_impl((void*)fn, value);
    }

    void invoke(void (*fn)()) {
        //invoke_impl((void*)fn, NULL, 0);
    }

    template<typename R> std::shared_ptr<Promise<R>> invoke_future(std::function<R(void)> fn)
    {
        auto task = std::make_shared<Promise<R()>>(fn);
        Item& item = task;
        put(task);
        return task;
    }

};


template <size_t queue_size=50>
class ActiveObjectChannel : public ActiveObjectBase
{
    cpp::channel<Item, queue_size> _channel;

protected:

    virtual bool take(Item& item) override
    {
        return cpp::select().recv_only(_channel, item).try_once();
    }

    virtual void put(const Item& item) override
    {
        _channel.send(item);
    }


public:

    ActiveObjectChannel(ActiveObjectConfiguration& config) : ActiveObjectBase(config) {}

    /**
     * Start the asynchronous processing for this active object.
     */
    void start()
    {
        _channel = cpp::channel<Item*, queue_size>();
        start_thread();
    }

};

class ActiveObjectQueue : public ActiveObjectBase
{
    os_queue_t  queue;

protected:

    virtual bool take(Item& result)
    {
        return os_queue_take(queue, &result, configuration.take_wait);
    }

    virtual void put(Item& item)
    {
        // since the queue does a binary copy rather than using the assignment operator
        // we don't get the required shared pointer reference increment
        // force it by doing a placement new allocation and invoke the copy ctor
        uint8_t buf[sizeof(Item)];
        Item* pItem = new (buf) Item(item);     // placement new, and copy construct from item
        while (!os_queue_put(queue, pItem, CONCURRENT_WAIT_FOREVER)) {}
        // item is reset when removed from the queue.
    }

    void createQueue(int size=50)
    {
        os_queue_create(&queue, sizeof(Item*), size);
    }

public:

    ActiveObjectQueue(const ActiveObjectConfiguration& config) : ActiveObjectBase(config), queue(NULL) {}

    void start()
    {
        createQueue();
    }
};


class ActiveObjectCurrentThreadQueue : ActiveObjectQueue
{
public:
    ActiveObjectCurrentThreadQueue(const ActiveObjectConfiguration& config) : ActiveObjectQueue(config) {}

    void start()
    {
        createQueue();
        _thread_id = std::this_thread::get_id();
        run();
    }
};

class ActiveObjectThreadQueue : public ActiveObjectQueue
{

public:

    ActiveObjectThreadQueue(const ActiveObjectConfiguration& config) : ActiveObjectQueue(config) {}

    void start()
    {
        createQueue();
        start_thread();
    }


};



#endif