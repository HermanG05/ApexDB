#include "thread_pool.hpp"

#include <stdexcept>
#include <system_error>

static void *worker(void *arg) {
    auto *tp = static_cast<TheadPool *>(arg);
    for (;;) {
        pthread_mutex_lock(&tp->mu);
        while (tp->queue.empty() && !tp->stopping) {
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        }
        if (tp->queue.empty() && tp->stopping) {
            pthread_mutex_unlock(&tp->mu);
            return nullptr;
        }
        Work w = tp->queue.front();
        tp->queue.pop_front();
        pthread_mutex_unlock(&tp->mu);
        w.f(w.arg);
    }
}

void thread_pool_destroy(TheadPool *tp) {
    if (tp->threads.empty()) {
        return;
    }
    pthread_mutex_lock(&tp->mu);
    tp->stopping = true;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
    for (pthread_t thread : tp->threads) {
        pthread_join(thread, nullptr);
    }
    tp->threads.clear();
    pthread_cond_destroy(&tp->not_empty);
    pthread_mutex_destroy(&tp->mu);
}

void thread_pool_init(TheadPool *tp, size_t num_threads) {
    if (!tp->threads.empty() || num_threads == 0) {
        throw std::invalid_argument("thread pool requires a positive size and one initialization");
    }
    tp->threads.reserve(num_threads);
    int error = pthread_mutex_init(&tp->mu, nullptr);
    if (error) {
        throw std::system_error(error, std::generic_category());
    }
    error = pthread_cond_init(&tp->not_empty, nullptr);
    if (error) {
        pthread_mutex_destroy(&tp->mu);
        throw std::system_error(error, std::generic_category());
    }
    tp->stopping = false;
    for (size_t i = 0; i < num_threads; ++i) {
        pthread_t thread;
        error = pthread_create(&thread, nullptr, worker, tp);
        if (error) {
            if (tp->threads.empty()) {
                pthread_cond_destroy(&tp->not_empty);
                pthread_mutex_destroy(&tp->mu);
            } else {
                thread_pool_destroy(tp);
            }
            throw std::system_error(error, std::generic_category());
        }
        tp->threads.push_back(thread);
    }
}

void thread_pool_queue(TheadPool *tp, void (*f)(void *), void *arg) {
    if (tp->threads.empty()) {
        f(arg);
        return;
    }
    pthread_mutex_lock(&tp->mu);
    tp->queue.push_back(Work{f, arg});
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
}
